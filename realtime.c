#include "realtime.h"

/*
	realtime routers and engines:

		(currently only redis pubsub is supported as message dispatcher)

		sse -> start streaming SSE messages
		sseraw -> start streaming SSE messages as-is
		stream -> blindly send everything sent by the message dispatcher
		socket.io -> stream socket.io/engine.io messages in polling or websocket mode
		istream -> blindly stream whatever sent by the client to the message dispatcher
*/

extern struct uwsgi_server uwsgi;

struct uwsgi_offload_engine *realtime_redis_offload_engine;

static int realtime_redis_offload(struct wsgi_request *wsgi_req, char *channel, uint16_t channel_len, uint64_t custom) {
	struct uwsgi_offload_request uor;
	// empty channel ?
	if (channel_len == 0) return -1;
	uwsgi_offload_setup(realtime_redis_offload_engine, &uor, wsgi_req, 1);
        uor.name = "127.0.0.1:6379";
	uor.ubuf = uwsgi_buffer_new(uwsgi.page_size);
	// set message builder (use buf_pos as storage)
	uor.buf_pos = custom;
	if (uwsgi_buffer_append(uor.ubuf, "*2\r\n$9\r\nSUBSCRIBE\r\n$", 20)) goto error;
	if (uwsgi_buffer_num64(uor.ubuf, channel_len)) goto error;
	if (uwsgi_buffer_append(uor.ubuf, "\r\n", 2)) goto error;
	if (uwsgi_buffer_append(uor.ubuf, channel, channel_len)) goto error;
	if (uwsgi_buffer_append(uor.ubuf, "\r\n", 2)) goto error;
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

	if (!wsgi_req->headers_sent) {
		if (!wsgi_req->headers_size) {
			if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
			if (uwsgi_response_add_content_type(wsgi_req, "text/event-stream", 17)) goto end;
			if (uwsgi_response_add_header(wsgi_req, "Cache-Control", 13, "no-cache", 8)) goto end;
		}
		if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
	}

	if (!realtime_redis_offload(wsgi_req, ub->buf, ub->pos, ur->custom)) {
		wsgi_req->via = UWSGI_VIA_OFFLOAD;
		wsgi_req->status = 202;
	}
	uwsgi_buffer_destroy(ub);
end:
	return UWSGI_ROUTE_BREAK;
}

static int socketio_router_func(struct wsgi_request *wsgi_req, struct uwsgi_route *ur) {
	// only GET requests are managed
	if (uwsgi_strncmp(wsgi_req->method, wsgi_req->method_len, "GET", 3)) {
		return UWSGI_ROUTE_NEXT;
	}

        if (!wsgi_req->socket->can_offload) {
                uwsgi_log("[realtime] unable to use \"socketio\" router without offloading\n");
                return UWSGI_ROUTE_BREAK;
        }

        char **subject = (char **) (((char *)(wsgi_req))+ur->subject);
        uint16_t *subject_len = (uint16_t *)  (((char *)(wsgi_req))+ur->subject_len);

        struct uwsgi_buffer *ub = uwsgi_routing_translate(wsgi_req, ur, *subject, *subject_len, ur->data, ur->data_len);
        if (!ub) return UWSGI_ROUTE_BREAK;


	// sid ?
	uint16_t sid_len = 0;
	char *sid = uwsgi_get_qs(wsgi_req, "sid", 3, &sid_len);
	// HANDSHAKE ???
	if (!sid) {
		// first send headers (if needed)
		if (!wsgi_req->headers_sent) {
			if (!wsgi_req->headers_size) {
				if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
                       		if (uwsgi_response_add_content_type(wsgi_req, "application/octet-stream", 24)) goto end;
                       		if (uwsgi_response_add_header(wsgi_req, "Access-Control-Allow-Origin", 27, "*", 1)) goto end;
                       		if (uwsgi_response_add_header(wsgi_req, "Connection", 10, "Keep-Alive", 10)) goto end;
			}
			if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;	
		}
		// if no body has been sent, let's generate the json
		// {"sid":"SID","upgrades":["polling"],"pingTimeout":30000}
		if (wsgi_req->response_size == 0) {
			struct uwsgi_buffer *ubody = uwsgi_buffer_new(uwsgi.page_size);
			if (uwsgi_buffer_append(ubody, "0{\"sid\":\"", 9)) goto error;
			if (uwsgi_buffer_append(ubody, ub->buf, ub->pos)) goto error;
			//if (uwsgi_buffer_append(ubody, "\",\"upgrades\":[\"polling\"],\"pingTimeout\":90000}", 45)) goto error;
			if (uwsgi_buffer_append(ubody, "\",\"upgrades\":[],\"pingInterval\":60000,\"pingTimeout\":90000}", 57)) goto error;
			if (eio_build(ubody)) goto error;
			uwsgi_response_write_body_do(wsgi_req, ubody->buf, ubody->pos);
error:
			uwsgi_buffer_destroy(ubody);
		}
		goto end;
	}

	// first send headers (if needed)
        if (!wsgi_req->headers_sent) {
        	if (!wsgi_req->headers_size) {
                	if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
                        if (uwsgi_response_add_content_type(wsgi_req, "application/octet-stream", 24)) goto end;
                        if (uwsgi_response_add_header(wsgi_req, "Access-Control-Allow-Origin", 27, "*", 1)) goto end;
                        if (uwsgi_response_add_header(wsgi_req, "Connection", 10, "Keep-Alive", 10)) goto end;
			if (uwsgi_response_add_content_length(wsgi_req, 5)) goto end;
                }
                if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
        }

	if (wsgi_req->response_size == 0) {
		struct uwsgi_buffer *ubody = uwsgi_buffer_new(uwsgi.page_size);
		if (uwsgi_buffer_append(ubody, "40", 2)) goto error2;
		if (eio_build(ubody)) goto error2;
		uwsgi_response_write_body_do(wsgi_req, ubody->buf, ubody->pos);
		uwsgi_buffer_destroy(ubody);
		goto offload;
error2:
		uwsgi_buffer_destroy(ubody);
		goto end;
	}

offload:
        if (!realtime_redis_offload(wsgi_req, ub->buf, ub->pos, ur->custom)) {
                wsgi_req->via = UWSGI_VIA_OFFLOAD;
                wsgi_req->status = 202;
        }
end:
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

        if (!realtime_redis_offload(wsgi_req, ub->buf, ub->pos, 0)) {
                wsgi_req->via = UWSGI_VIA_OFFLOAD;
                wsgi_req->status = 202;
        }
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
        return 0;
}

static void realtime_register() {
	realtime_redis_offload_engine = uwsgi_offload_register_engine("realtime-redis", realtime_redis_offload_engine_prepare, realtime_redis_offload_engine_do);
	uwsgi_register_router("sse", sse_router);
	uwsgi_register_router("sseraw", sseraw_router);
	uwsgi_register_router("stream", stream_router);
	//uwsgi_register_router("istream", istream_router);
	uwsgi_register_router("socket.io", socketio_router);
}

struct uwsgi_plugin realtime_plugin = {
	.name = "realtime",
	.on_load = realtime_register,
};
