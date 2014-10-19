#include "realtime.h"

ssize_t realtime_chunked_parse(struct uwsgi_buffer *ub, char **chunk, size_t *chunk_len) {
	char *buf = ub->buf;
        size_t len = ub->pos;
	if (len < 3) return 0;

	size_t i;
	// wait for \r\n
	size_t blackslash_r = 0;
	for(i=0;i<len;i++) {
		if (blackslash_r == 0) {
			if (buf[i] == '\r') {
				blackslash_r = i;
				continue;
			}
		}
		else {
			if (buf[i] == '\n') {
				// strtoul will stop at the first \r
				size_t num =  strtoul(buf, NULL, 16);
				// end chunk ?
				if (num == 0) return -1;
				// enough bytes ?
				if ((len - (i+1)) < (num + 2)) return 0;
				*chunk = buf + i + 1;
				*chunk_len = num;
				return i + 1 + num + 2;
			}
			blackslash_r = 0;
		}
	}

	return 0;
}

/*
        - offload engine for chunked input stream

        fd -> connection for publish 
        s -> client connection

        status 0 -> waiting for fd to be opened
        status 1 -> wait for s and fd connection

        status 2 -> publish to redis
*/
int realtime_istream_chunked_offload_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {

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
				if (uwsgi_buffer_ensure(uor->ubuf1, 4096)) return -1;
                                ssize_t rlen = read(uor->s, uor->ubuf1->buf + uor->ubuf1->pos, 4096);
                                if (rlen > 0) {
					uor->ubuf1->pos += rlen;
					// check if we have a full chunk
					char *chunk = NULL;
					size_t chunk_len = 0;
					ssize_t chunked_rlen = realtime_chunked_parse(uor->ubuf1, &chunk, &chunk_len);
					if (chunked_rlen < 0) return -1;
					// more data
					if (chunked_rlen == 0) return 0;
					// publish !!
                                        uor->status = 2;
                                        uor->ubuf->pos = 0;
                                        uor->written = 0;
                                        if (realtime_redis_build_publish(uor->ubuf, chunk, chunk_len, "uwsgi", 5)) return -1;
					if (uwsgi_buffer_decapitate(uor->ubuf1, chunked_rlen)) return -1;
                                        if (event_queue_del_fd(ut->queue, uor->s, event_queue_read())) return -1;
                                        if (event_queue_fd_read_to_write(ut->queue, uor->fd)) return -1;
                                        return 0;
                                }
                                else if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_istream_chunked_offload_do() -> read()");
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
                                        uwsgi_error("realtime_istream_chunked_offload_do() -> read()");
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
                                        uwsgi_error("realtime_istream_chunked_offload_do() -> write()");
                                }
                        }
                        return -1;
                default:
                        return -1;
        }
        return -1;
}

