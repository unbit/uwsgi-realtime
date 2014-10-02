#include "realtime.h"

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
							if (uor->buf_pos == REALTIME_SSE) {
								uint64_t final_len = 0;
								uor->buf = sse_build(message, message_len, &final_len);
								if (!uor->buf) return -1;
								message_len = final_len;
							}
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
							if (event_queue_del_fd(ut->queue, uor->fd, event_queue_read())) return -1;\
                                        		if (event_queue_fd_read_to_write(ut->queue, uor->s)) return -1;
                                        		uor->status = 3;
						}
						if (uwsgi_buffer_decapitate(uor->ubuf, ret)) return -1;
						// again
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
			else if (fd == uor->s) {
				char buf[4096];
				ssize_t rlen;
				switch(uor->buf_pos) {
					// in socket.io mode, data from client are consumed
					case REALTIME_SOCKETIO:
						rlen = read(uor->s, buf, 4096);
						if (rlen <= 0) return -1;
						// again
						return 0;
					// in websocket mode, read a packet and forward
					// to redis
					default:
						break;
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
