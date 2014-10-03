#include "realtime.h"

int realtime_subscribe_ubuf(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {
	if (fd == uor->fd) {
		ssize_t rlen = write(uor->fd, uor->ubuf->buf + uor->written, uor->ubuf->pos - uor->written);
		if (rlen > 0) {
			uor->written += rlen;
			if (uor->written >= (size_t) uor->ubuf->pos) {
				// reset buffer
				uor->ubuf->pos = 0;
				uor->status = 2;
				if (event_queue_add_fd_read(ut->queue, uor->s))
					return -1;
				if (event_queue_fd_write_to_read(ut->queue, uor->fd))
					return -1;
			}
			return 0;
		}
		else if (rlen < 0) {
			uwsgi_offload_retry uwsgi_error("realtime_sse_offload_do() -> write()");
		}
	}
	return -1;
}

int realtime_write_buf(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor) {
	// forward the message to the client
	ssize_t rlen = write(uor->s, uor->buf + uor->pos, uor->to_write);
	if (rlen > 0) {
		uor->to_write -= rlen;
		uor->pos += rlen;
		if (uor->to_write == 0) {
			if (event_queue_fd_write_to_read(ut->queue, uor->s))
				return -1;
			if (event_queue_add_fd_read(ut->queue, uor->fd))
				return -1;
			uor->status = 2;
		}
		return 0;
	}
	else if (rlen < 0) {
		uwsgi_offload_retry uwsgi_error("realtime_sse_offload_do() -> write()/s");
	}
	return -1;
}

int realtime_upload_offload_engine_prepare(struct wsgi_request *wsgi_req, struct uwsgi_offload_request *uor) {
        return 0;
}

int realtime_redis_offload_engine_prepare(struct wsgi_request *wsgi_req, struct uwsgi_offload_request *uor) {
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
	status 0 -> wait for input data, and write to file
	status 1 -> write HTTP response
*/
#define HTTP_UPLOAD_RESPONSE "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: 2\r\n\r\nOK"
int realtime_upload_offload_engine_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {
	// setup
        if (fd == -1) {
                event_queue_add_fd_read(ut->queue, uor->s);
                return 0;
        }

	if (fd != uor->s) return -1;

	if (uor->status == 0) {
		ssize_t rlen = read(uor->s, uor->buf, UMIN((size_t)uor->buf_pos, uor->to_write));
		if (rlen > 0) {
			uor->to_write -= rlen;
			ssize_t wlen = write(uor->fd, uor->buf, rlen);
			if (wlen != rlen) return -1;
			if (uor->to_write == 0) {
				if (event_queue_fd_read_to_write(ut->queue, uor->s)) return -1;
				uor->status = 1;
				uor->to_write = sizeof(HTTP_UPLOAD_RESPONSE) -1;
				uor->pos = 0;
				memcpy(uor->buf, HTTP_UPLOAD_RESPONSE, sizeof(HTTP_UPLOAD_RESPONSE) -1);
				return 0;
			}
			return 0;
		}
		if (rlen < 0) {
			uwsgi_offload_retry uwsgi_error("realtime_upload_offload_engine_do() -> read()/s");
		}
	}
	else if (uor->status == 1) {
		ssize_t wlen = write(uor->s, uor->buf + uor->pos, uor->to_write - uor->pos);
		if (wlen > 0) {
			uor->pos += wlen;
			if ((size_t)uor->pos >= uor->to_write) {
				return -1;
			}
		}
		if (wlen < 0) {
			uwsgi_offload_retry uwsgi_error("realtime_upload_offload_engine_do() -> write()/s");
		}
	}

	return -1;
}

/*
	Here we wait for messages on a redis channel.
	Whenever a message is received it is forwarded as SSE, websocket packet
	or whatever you need

	status:
                0 -> waiting for connection on fd (redis)
                1 -> sending subscribe request to fd (redis, write event)
                2 -> start waiting for read on s (client) and fd (redis)
                3 -> write to s (client)
*/
int realtime_redis_offload_engine_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {
	ssize_t rlen;

	// setup
	if (fd == -1) {
		event_queue_add_fd_write(ut->queue, uor->fd);
		return 0;
	}

	if (uor->buf_pos == REALTIME_WEBSOCKET) {
		return realtime_websocket_offload_do(ut, uor, fd);
	}

	if (uor->buf_pos == REALTIME_ISTREAM) {
		return realtime_istream_offload_do(ut, uor, fd);
	}

	if (uor->buf_pos == REALTIME_SSE) {
		return realtime_sse_offload_do(ut, uor, fd);
	}

	// raw
	switch (uor->status) {
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
		return realtime_subscribe_ubuf(ut, uor, fd);
		// read event from s or fd
	case 2:
		if (fd == uor->fd) {
			// ensure ubuf is big enough
			if (uwsgi_buffer_ensure(uor->ubuf, 4096))
				return -1;
			rlen = read(uor->fd, uor->ubuf->buf + uor->ubuf->pos, 4096);
			if (rlen > 0) {
				uor->ubuf->pos += rlen;
				// check if we have a full redis message
				int64_t message_len = 0;
				char *message;
				ssize_t ret = urt_redis_pubsub(uor->ubuf->buf, uor->ubuf->pos, &message_len, &message);
				if (ret > 0) {
					if (message_len > 0) {
						if (uor->buf)
							free(uor->buf);
						else if (uor->buf_pos == REALTIME_SOCKETIO) {
							struct uwsgi_buffer *tmp_ub = uwsgi_buffer_new(message_len);
							if (uwsgi_buffer_append(tmp_ub, message, message_len)) {
								uwsgi_buffer_destroy(tmp_ub);
								return -1;
							}
							if (eio_build_http(tmp_ub)) {
								uwsgi_buffer_destroy(tmp_ub);
								return -1;
							}
							uor->buf = tmp_ub->buf;
							message_len = tmp_ub->pos;
							tmp_ub->buf = NULL;
							uwsgi_buffer_destroy(tmp_ub);
						}
						else {
							uor->buf = uwsgi_concat2n(message, message_len, "", 0);
						}
						uor->to_write = message_len;
						uor->pos = 0;
						if (event_queue_del_fd(ut->queue, uor->fd, event_queue_read()))
							return -1;
						if (event_queue_fd_read_to_write(ut->queue, uor->s))
							return -1;
						uor->status = 3;
					}
					if (uwsgi_buffer_decapitate(uor->ubuf, ret))
						return -1;
					// again
					ret = 0;
				}
				// 0 -> again -1 -> error
				return ret;
			}
			if (rlen < 0) {
				uwsgi_offload_retry uwsgi_error("realtime_redis_offload_engine_do() -> read()/fd");
			}
		}
		// an event from the client can only mean disconneciton
		return -1;
		// write event on s
	case 3:
		return realtime_write_buf(ut, uor);
	default:
		break;
	}

	return -1;

}
