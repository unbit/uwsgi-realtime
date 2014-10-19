#include "realtime.h"

extern struct uwsgi_offload_engine *realtime_interleaved_offload_engine;

// TODO move it to generic enegine
int realtime_interleaved_offload(struct wsgi_request *wsgi_req, char *rtp_socket) {
        struct uwsgi_offload_request uor;
        uwsgi_offload_setup(realtime_interleaved_offload_engine, &uor, wsgi_req, 1);
        uor.fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (uor.fd < 0) return -1;
	uwsgi_socket_nb(uor.fd);
	size_t argc = 0;
	char **argv = uwsgi_split_quoted(rtp_socket, strlen(rtp_socket), ";", &argc);
	if (!argv || argc == 0) {
		close(uor.fd);
		return -1;
	}
	uor.buf = uwsgi_malloc(sizeof(struct sockaddr_in) * argc);
	size_t i;
	for(i=0;i<argc;i++) {
		char *colon = strchr(argv[i], ':');
		if (!colon) {
			close(uor.fd);
			size_t j;
			for(j=i;j<argc;j++) free(argv[j]);
			free(argv);
			return -1;
		}
		*colon = 0;
		struct sockaddr_in *sin = (struct sockaddr_in *)uor.buf;
		socket_to_in_addr(argv[i], colon, 0, &sin[i]);
		free(argv[i]);
	}
	free(argv);
	uor.ubuf = uwsgi_buffer_new(4096);
	// how many channels ?
	uor.buf_pos = argc;
        return uwsgi_offload_run(wsgi_req, &uor, NULL);
}

int interleaved_router_func(struct wsgi_request *wsgi_req, struct uwsgi_route *ur) {
        if (!wsgi_req->socket->can_offload) {
                uwsgi_log("[realtime] unable to use \"interleaved\" router without offloading\n");
                return UWSGI_ROUTE_BREAK;
        }

        char **subject = (char **) (((char *)(wsgi_req))+ur->subject);
        uint16_t *subject_len = (uint16_t *)  (((char *)(wsgi_req))+ur->subject_len);

        struct uwsgi_buffer *ub = uwsgi_routing_translate(wsgi_req, ur, *subject, *subject_len, ur->data, ur->data_len);
        if (!ub) return UWSGI_ROUTE_BREAK;

        // keyval ?
        if (!strchr(ub->buf, '=')) {
        }
        else {
        }

        if (!realtime_interleaved_offload(wsgi_req, ub->buf)) {
                wsgi_req->via = UWSGI_VIA_OFFLOAD;
                wsgi_req->status = 202;
        }

        uwsgi_buffer_destroy(ub);
        return UWSGI_ROUTE_BREAK;
}

static ssize_t realtime_interleaved_parse(struct uwsgi_buffer *ub, uint8_t *channel, char **rtp, size_t *rtp_len) {
	char *buf = ub->buf;
        size_t len = ub->pos;

        if (len < 1)
                return 0;	
	
	// if packet starts with '$' it is an interleaved one, otherwise continue
	// reading until \r\n\r\n
	if (*buf == '$') goto interleaved;

	int found = 0;
	size_t i;

	for(i=0;i<len;i++) {
		char ptr = buf[i];
		switch(found) {
			case 0:
				if (ptr == '\r') {
					found = 1;
				}
				break;
			case 1:
				if (ptr == '\n') {
					found = 2;
					break;
				}
				found = 0;
				break;
			case 2:
				if (ptr == '\r') {
					found = 3;
					break;
				}
				found = 0;
				break;
			case 3:
				if (ptr == '\n') {
					return i;	
                                }
                                found = 0;
                                break;
		}
	}

	return 0;

interleaved:

	if (len < 4) return 0;

	*channel = buf[1];
	uint16_t pktsize = uwsgi_be16(buf + 2);

	if (len < (size_t) (4 + pktsize)) return 0;

	*rtp = buf + 4;
	*rtp_len = pktsize;
	
	return 4 + pktsize;
}

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

/*
        - offload engine for interleaved stream

        fd -> udp socket
        s -> client connection

        status 0 -> wait for s and write to udp

        status 1 -> write rtp packet
*/
int realtime_interleaved_offload_engine_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {

	// setup
	if (fd == -1) {
		if (event_queue_add_fd_read(ut->queue, uor->s)) return -1;
		return 0;
	}

        if (uor->s != fd) return -1;

                                // data from publish channel (consume, end on error)
                                if (uwsgi_buffer_ensure(uor->ubuf, 4096)) return -1;
                                ssize_t rlen = read(uor->s, uor->ubuf->buf + uor->ubuf->pos, 4096);
                                if (rlen == 0) return -1;
                                if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_interleaved_offload_do() -> read()");
                                        return -1;
                                }
                                uor->ubuf->pos += rlen;
				uint8_t channel = 0;
                                char *rtp = NULL;
                                size_t rtp_len = 0;
                                ssize_t ret = realtime_interleaved_parse(uor->ubuf, &channel, &rtp, &rtp_len);
                                if (ret > 0) {
					struct sockaddr_in *sin = (struct sockaddr_in *) uor->buf;
					if (channel > 0) {
						// even ?
						if (channel % 2 != 0) goto skip;
						channel = channel/2;
						if (channel >= uor->buf_pos) goto skip;
					}
					ssize_t wlen = sendto(uor->fd, rtp, rtp_len, 0, (struct sockaddr *) &sin[channel], sizeof(struct sockaddr_in));
					if (wlen != (ssize_t)rtp_len) {
						if (wlen < 0 && uwsgi_is_again()) return 0;
						uwsgi_error("realtime_interleaved_offload_engine_do()/sendto()");
						return -1;
					}
skip:
                                       	if (uwsgi_buffer_decapitate(uor->ubuf, ret)) return -1;
                                        return 0;
                                }
                                return ret;
}
