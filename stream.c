#include "realtime.h"

/*
	- offload engine for input stream

	fd -> connection for publish 
	s -> client connection

	status 0 -> waiting for fd to be opened
	status 1 -> wait for s and fd connection

	status 2 -> publish to redis
*/
int realtime_istream_offload_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {

	// TODO make this buffer configurable
	char buf[8192];

        switch(uor->status) {
                // waiting for fd connection
                case 0:
                        if (fd == uor->fd) {
                                uor->status = 1;
				if (event_queue_fd_write_to_read(ut->queue, uor->fd)) return -1;
				if (event_queue_add_fd_read(ut->queue, uor->s)) return -1;
                                return 0;
                        }
                        return -1;
		// wait for s and fd
		case 1:
			if (fd == uor->s) {
                                ssize_t rlen = read(uor->s, buf, 8192);
                                if (rlen > 0) {
					uor->status = 2;
					uor->ubuf->pos = 0;
					uor->written = 0;
					if (realtime_redis_build_publish(uor->ubuf, buf, rlen, "uwsgi", 5)) return -1;
					if (event_queue_del_fd(ut->queue, uor->s, event_queue_read())) return -1;
					if (event_queue_fd_read_to_write(ut->queue, uor->fd)) return -1;
                                        return 0;
                                }
                                else if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_istream_offload_do() -> read()");
                                }
				return -1;
                        }

			if (uor->fd == fd) {
				// data from publish channel (consume, end on error)
                                if (uwsgi_buffer_ensure(uor->ubuf, 4096)) return -1;
                                ssize_t rlen = read(uor->fd, uor->ubuf->buf + uor->ubuf->pos, 4096);
                                if (rlen == 0) return -1;
                                if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_istream_offload_do() -> read()");
                                        return -1;
                                }
                                uor->ubuf->pos += rlen;
				char array_type;
				char *array;
				int64_t array_len;
                                ssize_t ret = urt_redis_parse(uor->ubuf->buf, uor->ubuf->pos, &array_type, &array_len, &array);
                                if (ret > 0) {
                                        if (uwsgi_buffer_decapitate(uor->ubuf, ret)) return -1;
                                        return 0;
                                }
                                return ret;
                        }
                        return -1;
		case 2:
			// publish received data to redis
			if (fd == uor->fd) {
                                ssize_t rlen = write(uor->fd, uor->ubuf->buf + uor->written, uor->ubuf->pos-uor->written);
                                if (rlen > 0) {
                                        uor->written += rlen;
                                        if (uor->written >= (size_t)uor->ubuf->pos) {
                                                // reset buffer
                                                uor->ubuf->pos = 0;
						// back to wait
                                                uor->status = 1;
                                                if (event_queue_add_fd_read(ut->queue, uor->s)) return -1;
						if (event_queue_fd_write_to_read(ut->queue, uor->fd)) return -1;
                                        }
                                        return 0;
                                }
                                else if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_istream_offload_do() -> write()");
                                }
                        }
			return -1;
		default:
			return -1;
	}
	return -1;
}
