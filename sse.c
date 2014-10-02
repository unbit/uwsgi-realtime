#include "realtime.h"

char *sse_build(char *message, int64_t message_len, uint64_t *final_len) {
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

int realtime_sse_offload_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {
	ssize_t rlen;

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
                                        uwsgi_error("realtime_sse_offload_do() -> write()");
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
							uint64_t final_len = 0;
							uor->buf = sse_build(message, message_len, &final_len);
							if (!uor->buf) return -1;
							message_len = final_len;
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
                                        uwsgi_error("realtime_sse_offload_do() -> read()/fd");
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
                                uwsgi_error("realtime_sse_offload_do() -> write()/s");
                        }
                        return -1;
		default:
                        break;
        }

        return -1;
		
}
