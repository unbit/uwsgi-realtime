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

#define uwsgi_offload_retry if (uwsgi_is_again()) return 0;
#define uwsgi_offload_0r_1w(x, y) if (event_queue_del_fd(ut->queue, x, event_queue_read())) return -1;\
                                        if (event_queue_fd_read_to_write(ut->queue, y)) return -1;

extern struct uwsgi_server uwsgi;

struct uwsgi_offload_engine *realtime_redis_offload_engine;

static char *sse_build(char *message, int64_t message_len, uint64_t *final_len) {
	int64_t i;
	struct uwsgi_buffer *ub = uwsgi_buffer_new(message_len);
	char *ptr = message;
	size_t len = 0;
	for(i=0;i<message_len;i++) {
		len++;
		if (message[i] == '\n') {
			if (uwsgi_buffer_append(ub, "data: ", 6)) goto error;
			if (uwsgi_buffer_append(ub, ptr, len)) goto error;
			ptr = message+i+1;
			len = 0;
		}
	}

	if (uwsgi_buffer_append(ub, "data: ", 6)) goto error;
	if (len > 0) {
		if (uwsgi_buffer_append(ub, ptr, len)) goto error;
	}
	if (uwsgi_buffer_append(ub, "\n\n", 2)) goto error;
	*final_len = ub->pos;
	char *buf = ub->buf;
	ub->buf = NULL;
	uwsgi_buffer_destroy(ub);
	return buf;
error:
	uwsgi_buffer_destroy(ub);
	return NULL;
}

static int realtime_redis_offload(struct wsgi_request *wsgi_req, char *channel, uint16_t channel_len) {
	struct uwsgi_offload_request uor;
	// empty channel ?
	if (channel_len == 0) return -1;
	uwsgi_offload_setup(realtime_redis_offload_engine, &uor, wsgi_req, 1);
        uor.name = "127.0.0.1:6379";
	uor.ubuf = uwsgi_buffer_new(uwsgi.page_size);
	// set sse as the message builder
	uor.buf_pos = 1;
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

	if (!wsgi_req->headers_sent) {
		if (!wsgi_req->headers_size) {
			if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
			if (uwsgi_response_add_content_type(wsgi_req, "text/event-stream", 17)) goto end;
			if (uwsgi_response_add_header(wsgi_req, "Cache-Control", 13, "no-cache", 8)) goto end;
		}
		if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
	}

	if (!realtime_redis_offload(wsgi_req, "uwsgi", 5)) {
		wsgi_req->via = UWSGI_VIA_OFFLOAD;
		wsgi_req->status = 202;
	}
end:
	return UWSGI_ROUTE_BREAK;
}

static int sse_router(struct uwsgi_route *ur, char *args) {
	ur->func = sse_router_func;
	ur->data = args;
	ur->data_len = strlen(args);
	return 0;
}

static int realtime_redis_offload_engine_prepare(struct wsgi_request *wsgi_req, struct uwsgi_offload_request *uor) {
	if (!uor->name) {
                return -1;
        }

        uor->fd = uwsgi_connect(uor->name, 0, 1);
        if (uor->fd < 0) {
                uwsgi_error("realtime_redis_offload_engine_prepare()/connect()");
                return -1;
        }

        return 0;
}

/*
	Here we wait for messages on a redis channel.
	Whenever a message is received it is forwarded as SSE

	status:
                0 -> waiting for connection on fd (redis)
                1 -> sending subscribe request to fd (redis, write event)
                2 -> start waiting for read on s (client) and fd (redis)
                3 -> write to s (client)
*/
static int realtime_redis_offload_engine_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {
	ssize_t rlen;

        // setup
        if (fd == -1) {
                event_queue_add_fd_write(ut->queue, uor->fd);
                return 0;
        }

	switch(uor->status) {
                // waiting for connection
                case 0:
                        if (fd == uor->fd) {
                                uor->status = 1;
                                // ok try to send the request right now...
                                return realtime_redis_offload_engine_do(ut, uor, fd);
                        }
                        return -1;
		// writing the SUBSCRIBE request
		case 1:
                        if (fd == uor->fd) {
                                rlen = write(uor->fd, uor->ubuf->buf + uor->written, uor->ubuf->pos-uor->written);
                                if (rlen > 0) {
                                        uor->written += rlen;
                                        if (uor->written >= (size_t)uor->ubuf->pos) {
						// reset buffer
						uor->ubuf->pos = 0;
                                                uor->status = 2;
                                                if (event_queue_add_fd_read(ut->queue, uor->s)) return -1;
                                                if (event_queue_fd_write_to_read(ut->queue, uor->fd)) return -1;
                                        }
                                        return 0;
                                }
                                else if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_redis_offload_engine_do() -> write()");
                                }
                        }
                        return -1;
		// read event from s or fd
                case 2:
                        if (fd == uor->fd) {
				// ensure ubuf is big enough
				if (uwsgi_buffer_ensure(uor->ubuf, 4096)) return -1;
                                rlen = read(uor->fd, uor->ubuf->buf + uor->ubuf->pos, 4096);
                                if (rlen > 0) {
					uor->ubuf->pos += rlen;
					// check if we have a full redis message
					int64_t message_len = 0;
					char *message;
					ssize_t ret = urt_redis_pubsub(uor->ubuf->buf, uor->ubuf->pos, &message_len, &message);
					if (ret > 0) {
						if (message_len > 0) {
							if (uor->buf) free(uor->buf);
							// what to do with the message ?
							// buf_pos is used as the type (yes, it is ugly, sorry)
							if (uor->buf_pos == 1) {
								uint64_t final_len = 0;
								uor->buf = sse_build(message, message_len, &final_len);
								if (!uor->buf) return -1;
								message_len = final_len;
							}
							else {
								uor->buf = uwsgi_concat2n(message, message_len, "", 0);
							}
                                        		uor->to_write = message_len;
                                        		uor->pos = 0;
                                        		uwsgi_offload_0r_1w(uor->fd, uor->s)
                                        		uor->status = 3;
						}
						if (uwsgi_buffer_decapitate(uor->ubuf, ret)) return -1;
						ret = 0;
					}
					// 0 -> again -1 -> error
                                        return ret;
                                }
                                if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_redis_offload_engine_do() -> read()/fd");
                                }
                        }
			// an event from the client can only mean disconneciton
                        return -1;
		// write event on s
                case 3:
			// forward the message to the client
                        rlen = write(uor->s, uor->buf + uor->pos, uor->to_write);
                        if (rlen > 0) {
                                uor->to_write -= rlen;
                                uor->pos += rlen;
                                if (uor->to_write == 0) {
                                        if (event_queue_fd_write_to_read(ut->queue, uor->s)) return -1;
                                        if (event_queue_add_fd_read(ut->queue, uor->fd)) return -1;
                                        uor->status = 2;
                                }
                                return 0;
                        }
                        else if (rlen < 0) {
                                uwsgi_offload_retry
                                uwsgi_error("realtime_redis_offload_engine_do() -> write()/s");
                        }
                        return -1;
		default:
                        break;
        }

        return -1;
		
}

static void realtime_register() {
	realtime_redis_offload_engine = uwsgi_offload_register_engine("realtime-redis", realtime_redis_offload_engine_prepare, realtime_redis_offload_engine_do);
	uwsgi_register_router("sse", sse_router);
	uwsgi_register_router("sseraw", sse_router);
}

struct uwsgi_plugin realtime_plugin = {
	.name = "realtime",
	.on_load = realtime_register,
};
