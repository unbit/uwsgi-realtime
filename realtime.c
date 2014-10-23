#include "realtime.h"

/*
	realtime routers and engines:

		(currently only redis pubsub is supported as message dispatcher)

		sse -> start streaming SSE messages
		sseraw -> start streaming SSE messages as-is
		socket.io -> stream socket.io/engine.io messages in polling or websocket mode
		stream -> blindly send everything sent by the message dispatcher
		istream -> blindly stream whatever sent by the client to the message dispatcher (supports chunked input)
		websocket -> send received websockets packet to message dispatcher, send received messagess from the message diaptcher to the client as websocket packets
		upload -> store the request input to a file
		webm -> send valid webm header and then start broadcasting video chunks from the message dispatcher
		interleaved
		mjpg/mjpeg
		mpng
		rtsp
*/

extern struct uwsgi_server uwsgi;

struct uwsgi_offload_engine *realtime_redis_offload_engine;
struct uwsgi_offload_engine *realtime_upload_offload_engine;
struct uwsgi_offload_engine *realtime_interleaved_offload_engine;

void realtime_destroy_config(struct realtime_config *rc) {
	if (rc->server) free(rc->server);
	if (rc->publish) free(rc->publish);
	if (rc->subscribe) free(rc->subscribe);
	if (rc->prefix) free(rc->prefix);
	if (rc->suffix) free(rc->suffix);
	if (rc->boundary) free(rc->boundary);
	if (rc->buffer_size_str) free(rc->buffer_size_str);
	if (rc->video_demuxer) free(rc->video_demuxer);
	if (rc->sid) free(rc->sid);
	if (rc->src) free(rc->src);
	if (rc->dst) free(rc->dst);
	if (rc->tmp) free(rc->tmp);
	if (rc->video_codec) free(rc->video_codec);
	if (rc->audio_codec) free(rc->audio_codec);
	free(rc);
}

static void realtime_offload_destroy_config(struct uwsgi_offload_request *uor) {
	realtime_destroy_config((struct realtime_config *) uor->data);
}

int realtime_redis_offload(struct wsgi_request *wsgi_req, struct realtime_config *rc) {
	struct uwsgi_offload_request uor;
	// empty channel ?
	uwsgi_offload_setup(realtime_redis_offload_engine, &uor, wsgi_req, 1);
	if (!rc->server) {
		rc->server = uwsgi_str("127.0.0.1:6379");
	}
	if (rc->publish) {
		rc->publish_len = strlen(rc->publish);
	}
	if (rc->prefix) {
		rc->prefix_len = strlen(rc->prefix);
	}
	if (rc->suffix) {
		rc->suffix_len = strlen(rc->suffix);
	}
	if (rc->boundary) {
		rc->boundary_len = strlen(rc->boundary);
	}
	if (rc->buffer_size_str) {
		rc->buffer_size = atoi(rc->buffer_size_str);
	}
	if (!rc->buffer_size) {
		rc->buffer_size = 4096;
	}
	uor.data = rc;
        uor.name = rc->server;
	uor.ubuf = uwsgi_buffer_new(uwsgi.page_size);
	if (rc->engine == REALTIME_WEBSOCKET) {
		uor.ubuf1 = uwsgi_buffer_new(uwsgi.page_size);
		uor.ubuf2 = uwsgi_buffer_new(uwsgi.page_size);
		uor.ubuf3 = uwsgi_buffer_new(uwsgi.page_size);
	}
	else if (rc->engine == REALTIME_MJPEG || rc->engine == REALTIME_RTSP) {
		uor.ubuf1 = uwsgi_buffer_new(uwsgi.page_size);
		uor.ubuf3 = uwsgi_buffer_new(uwsgi.page_size);
	}

	if (rc->engine == REALTIME_RTSP) {
		uor.ubuf2 = uwsgi_buffer_new(uwsgi.page_size);
	}
	// TODO this should be applied to plain ISTREAM too
	if (rc->engine == REALTIME_ISTREAM_CHUNKED) {
		uor.ubuf1 = uwsgi_buffer_new(uwsgi.page_size);
		// 1 MB limit by default
		uor.ubuf1->limit = 1024 * 1024 * 1024;
        	// append remaining body...
        	if (wsgi_req->proto_parser_remains > 0) {
                	if (uwsgi_buffer_append(uor.ubuf1, wsgi_req->proto_parser_remains_buf, wsgi_req->proto_parser_remains)) {
                        	uwsgi_buffer_destroy(uor.ubuf1);
				goto error;
			}
                	wsgi_req->post_pos += wsgi_req->proto_parser_remains;
                	wsgi_req->proto_parser_remains = 0;
        	}
	}

	if (rc->subscribe) {
		if (uwsgi_buffer_append(uor.ubuf, "*2\r\n$9\r\nSUBSCRIBE\r\n$", 20)) goto error;
		if (uwsgi_buffer_num64(uor.ubuf, strlen(rc->subscribe))) goto error;
		if (uwsgi_buffer_append(uor.ubuf, "\r\n", 2)) goto error;
		if (uwsgi_buffer_append(uor.ubuf, rc->subscribe, strlen(rc->subscribe))) goto error;
		if (uwsgi_buffer_append(uor.ubuf, "\r\n", 2)) goto error;
	}
	uor.free = realtime_offload_destroy_config;
        return uwsgi_offload_run(wsgi_req, &uor, NULL);
error:
	uwsgi_buffer_destroy(uor.ubuf);
	return -1;
}

static int sse_router_func(struct wsgi_request *wsgi_req, struct uwsgi_route *ur) {
	if (!wsgi_req->socket->can_offload) {
		uwsgi_log("[realtime] unable to use \"sse\" router without offloading\n");
		return UWSGI_ROUTE_BREAK;
	}

	char **subject = (char **) (((char *)(wsgi_req))+ur->subject);
        uint16_t *subject_len = (uint16_t *)  (((char *)(wsgi_req))+ur->subject_len);

        struct uwsgi_buffer *ub = uwsgi_routing_translate(wsgi_req, ur, *subject, *subject_len, ur->data, ur->data_len);
        if (!ub) return UWSGI_ROUTE_BREAK;

	struct realtime_config *rc = uwsgi_calloc(sizeof(struct realtime_config));
	if (strchr(ub->buf, '=')) {
		if (uwsgi_kvlist_parse(ub->buf, ub->pos, ',', '=',
			"server", &rc->server,
			"subscribe", &rc->subscribe,
			"buffer_size", &rc->buffer_size_str,
			NULL)) {
			uwsgi_log("[realtime] unable to parse sse action\n");
			realtime_destroy_config(rc);
			uwsgi_buffer_destroy(ub);
			return UWSGI_ROUTE_BREAK;
		}
	}
	else {
		rc->subscribe = uwsgi_str(ub->buf);
	}

	if (!wsgi_req->headers_sent) {
		if (!wsgi_req->headers_size) {
			if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
			if (uwsgi_response_add_content_type(wsgi_req, "text/event-stream", 17)) goto end;
			if (uwsgi_response_add_header(wsgi_req, "Cache-Control", 13, "no-cache", 8)) goto end;
		}
		if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
	}


	rc->engine = ur->custom;
	if (!realtime_redis_offload(wsgi_req, rc)) {
		wsgi_req->via = UWSGI_VIA_OFFLOAD;
		wsgi_req->status = 202;
		rc = NULL;
	}

end:
	if (rc) realtime_destroy_config(rc);
	uwsgi_buffer_destroy(ub);
	return UWSGI_ROUTE_BREAK;
}

static int stream_router_func(struct wsgi_request *wsgi_req, struct uwsgi_route *ur) {
        if (!wsgi_req->socket->can_offload) {
                uwsgi_log("[realtime] unable to use \"stream\" router without offloading\n");
                return UWSGI_ROUTE_BREAK;
        }

        char **subject = (char **) (((char *)(wsgi_req))+ur->subject);
        uint16_t *subject_len = (uint16_t *)  (((char *)(wsgi_req))+ur->subject_len);

        struct uwsgi_buffer *ub = uwsgi_routing_translate(wsgi_req, ur, *subject, *subject_len, ur->data, ur->data_len);
        if (!ub) return UWSGI_ROUTE_BREAK;

	struct realtime_config *rc = uwsgi_calloc(sizeof(struct realtime_config));
	if (strchr(ub->buf, '=')) {
                if (uwsgi_kvlist_parse(ub->buf, ub->pos, ',', '=',
                        "server", &rc->server,
                        "subscribe", &rc->subscribe,
                        "publish", &rc->publish,
                        "suffix", &rc->suffix,
                        "prefix", &rc->prefix,
                        NULL)) {
                        uwsgi_log("[realtime] unable to parse stream action\n");
                        realtime_destroy_config(rc);
                        uwsgi_buffer_destroy(ub);
                        return UWSGI_ROUTE_BREAK;
                }
        }
        else {
                rc->subscribe = uwsgi_str(ub->buf);
        }

	if (!wsgi_req->headers_sent) {
		if (ur->custom == REALTIME_WEBSOCKET) {
			if (uwsgi_websocket_handshake(wsgi_req, NULL, 0, NULL, 0, NULL, 0)) goto end;
		}
		else {
                	if (!wsgi_req->headers_size) {
				if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;			
			}
			if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
		}
	}

	// do we need chunked encoding ?
	if (ur->custom == REALTIME_ISTREAM) {
		uint16_t transfer_encoding_len = 0;
		char *transfer_encoding = uwsgi_get_var(wsgi_req, "HTTP_TRANSFER_ENCODING", 22, &transfer_encoding_len);
		if (transfer_encoding) {
			if (!uwsgi_strncmp(transfer_encoding, transfer_encoding_len, "chunked", 7)) {
				ur->custom = REALTIME_ISTREAM_CHUNKED;
			}
		} 
	}

	rc->engine = ur->custom;
        if (!realtime_redis_offload(wsgi_req, rc)) {
                wsgi_req->via = UWSGI_VIA_OFFLOAD;
                wsgi_req->status = 202;
		rc = NULL;
        }
end:
	if (rc) realtime_destroy_config(rc);
        uwsgi_buffer_destroy(ub);
        return UWSGI_ROUTE_BREAK;
}

static int socketio_router(struct uwsgi_route *ur, char *args) {
        ur->func = socketio_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
	ur->custom = REALTIME_SOCKETIO;
        return 0;
}

static int sse_router(struct uwsgi_route *ur, char *args) {
	ur->func = sse_router_func;
	ur->data = args;
	ur->data_len = strlen(args);
	ur->custom = REALTIME_SSE;
	return 0;
}

static int sseraw_router(struct uwsgi_route *ur, char *args) {
        ur->func = sse_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
	ur->custom = REALTIME_RAW;
        return 0;
}

static int stream_router(struct uwsgi_route *ur, char *args) {
        ur->func = stream_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
	ur->custom = REALTIME_RAW;
        return 0;
}

static int webm_router(struct uwsgi_route *ur, char *args) {
        ur->func = webm_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
        ur->custom = REALTIME_RAW;
        return 0;
}

static int mjpeg_router(struct uwsgi_route *ur, char *args) {
        ur->func = mjpeg_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
        ur->custom = REALTIME_MJPEG;
        return 0;
}

static int istream_router(struct uwsgi_route *ur, char *args) {
        ur->func = stream_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
	ur->custom = REALTIME_ISTREAM;
        return 0;
}

static int interleaved_router(struct uwsgi_route *ur, char *args) {
        ur->func = interleaved_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
	ur->custom = REALTIME_INTERLEAVED;
        return 0;
}

static int rtsp_router(struct uwsgi_route *ur, char *args) {
        ur->func = rtsp_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
	ur->custom = REALTIME_RTSP;
        return 0;
}

static int websocket_router(struct uwsgi_route *ur, char *args) {
        ur->func = stream_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
	ur->custom = REALTIME_WEBSOCKET;
        return 0;
}

static int upload_router(struct uwsgi_route *ur, char *args) {
        ur->func = upload_router_func;
        ur->data = args;
        ur->data_len = strlen(args);
        return 0;
}

static void realtime_register() {
	realtime_redis_offload_engine = uwsgi_offload_register_engine("realtime-redis", realtime_redis_offload_engine_prepare, realtime_redis_offload_engine_do);
	realtime_upload_offload_engine = uwsgi_offload_register_engine("realtime-upload", realtime_upload_offload_engine_prepare, realtime_upload_offload_engine_do);
	realtime_interleaved_offload_engine = uwsgi_offload_register_engine("realtime-interleaved", realtime_interleaved_offload_engine_prepare, realtime_interleaved_offload_engine_do);
	uwsgi_register_router("sse", sse_router);
	uwsgi_register_router("sseraw", sseraw_router);
	uwsgi_register_router("stream", stream_router);
	uwsgi_register_router("istream", istream_router);
	uwsgi_register_router("socket.io", socketio_router);
	uwsgi_register_router("websocket", websocket_router);
	uwsgi_register_router("upload", upload_router);
	uwsgi_register_router("webm", webm_router);
	uwsgi_register_router("mjpeg", mjpeg_router);
	uwsgi_register_router("mjpg", mjpeg_router);
	uwsgi_register_router("rtsp", rtsp_router);
	uwsgi_register_router("interleaved", interleaved_router);
}

struct uwsgi_plugin realtime_plugin = {
	.name = "realtime",
	.on_load = realtime_register,
};
