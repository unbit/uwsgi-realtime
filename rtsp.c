#include "realtime.h"

extern struct uwsgi_server uwsgi;

/*

	RTSP methods:

		client sends ANNOUNCE with a body
		server answers 200

		client sends OPTIONS
		server answers with support for DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, RECORD

		client sends SETUP
		server answers with Transport header

		client sends RECORD
		server answers 200

		client sends PAUSE
		server answers 200

		client sends TEARDOWN
		server answers 200 and close the connection

		interleaved frames expected

*/

static int sprop_decode(struct uwsgi_buffer *ub, char *buf, size_t len) {
	size_t dst_len = 0;
	char *dst = uwsgi_base64_decode(buf, len, &dst_len);
	if (!dst) return -1;
	if (uwsgi_buffer_append(ub, "\0\0\0\1", 4)) goto error;
	if (uwsgi_buffer_append(ub, dst, dst_len)) goto error;
	free(dst);
	return 0;

error:
	free(dst);
	return -1;
}

static int sprop_parse(struct realtime_config *rc, char *buf, size_t len) {
	size_t i;
	char *b64 = buf;
	size_t b64_len = 0;
	for(i=0;i<len;i++) {
		if (buf[i] == ',') {
			if (sprop_decode(rc->sprop, b64, b64_len)) return -1;
			b64 = buf + i + 1;
			b64_len = 0;
		}
		else {
			b64_len++;
		}
	}

	if (b64_len > 0) {
		if (sprop_decode(rc->sprop, b64, b64_len)) return -1;
	}
	return 0;
}

int sdp_parse(struct realtime_config *rc, char *buf, size_t len) {
	size_t i;
	size_t line_len = 0;
	char *line = buf;
	rc->sprop = uwsgi_buffer_new(uwsgi.page_size);
	for(i=0;i<len;i++) {
		// end of the line ?
		if (buf[i] == '\n') {
			if (line_len > 0) {
				if (line[line_len-1] == '\r') line_len--;
			}
			if (line_len > 21) {
				line[line_len] = 0;
				char *sprop = strstr(line, "sprop-parameter-sets=");
				if (sprop) {
					size_t sprop_len = strlen(sprop);
					if (sprop_len > 21) {
						if (sprop_parse(rc, sprop+21, sprop_len-21)) return -1;
					}
				}
			}
			line = buf + i + 1;
			line_len = 0;
		}
		else {
			line_len++;
		}
	}
	return 0;
}

static int consume_request_body(struct wsgi_request *wsgi_req, struct realtime_config *rc) {
	ssize_t rlen = 0;
        char *buf = uwsgi_request_body_read(wsgi_req, wsgi_req->post_cl, &rlen);
        if (!buf) return -1;
        if (buf == uwsgi.empty) return -1;
	return sdp_parse(rc, buf, rlen);
}

static ssize_t rtsp_find_rnrn(char *buf, size_t len) {
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
                                        return i+1;
                                }
                                found = 0;
                                break;
                }
        }

	return 0;
}

// we are interested in the HTTP Method, the Content-Length, the Transport and the Cseq headers to build a response
static int rtsp_parse(char *buf, size_t len, char **method, size_t *method_len, size_t *cl, char **cseq, size_t *cseq_len, char **transport, size_t *transport_len) {
	char *ptr = buf;
	char *watermark = buf + len;
	char *base = ptr;

	// REQUEST_METHOD 
        while (ptr < watermark) {
                if (*ptr == ' ') {
			*method = base;
			*method_len = ptr - base;
                        ptr++;
                        break;
                }
                ptr++;
        }

        // REQUEST_URI / PATH_INFO / QUERY_STRING
        base = ptr;
        while (ptr < watermark) {
                if (*ptr == ' ') {
                        ptr++;
                        break;
                }
                ptr++;
        }

        // SERVER_PROTOCOL
        base = ptr;
        while (ptr < watermark) {
                if (*ptr == '\r') {
                        if (ptr + 1 >= watermark)
                                return -1 ;
                        if (*(ptr + 1) != '\n')
                                return -1;
                        if (ptr - base > 0xffff) return -1;
                        ptr += 2;
                        break;
                }
                ptr++;
        }

	// headers
	base = ptr;
	while (ptr < watermark) {
                if (*ptr == '\r') {
                        if (ptr + 1 >= watermark)
                                return -1;
                        if (*(ptr + 1) != '\n')
                                return -1;
                        // multiline header ?
                        if (ptr + 2 < watermark) {
                                if (*(ptr + 2) == ' ' || *(ptr + 2) == '\t') {
                                        ptr += 2;
                                        continue;
                                }
                        }
                        // last line, do not waste time
                        if (ptr - base == 0) break;
                        if (ptr - base > 0xffff) return -1;
			char *colon = memchr(base, ':', ptr - base);
			// invalid header ?
			if (!colon) return -1;
			if (!uwsgi_strnicmp(base, colon-base, "Cseq", 4)) {
				if ((colon-base) + 2 >= (ptr-base)) return -1;
				*cseq = colon + 2;
				*cseq_len = (ptr-base) - ((colon-base) + 2);
			}
			else if (!uwsgi_strnicmp(base, colon-base, "Content-Length", 14)) {
                                if ((colon-base) + 2 >= (ptr-base)) return -1;
                                *cl = uwsgi_str_num(colon + 2, (ptr-base) - ((colon-base) + 2));
                        }
			else if (!uwsgi_strnicmp(base, colon-base, "Transport", 9)) {
                                if ((colon-base) + 2 >= (ptr-base)) return -1;
                                *transport = colon + 2;
                                *transport_len = (ptr-base) - ((colon-base) + 2);
                        }
                        ptr++;
                        base = ptr + 1;
                }
                ptr++;
        }

	return 0;
}

static ssize_t rtsp_manage(struct realtime_config *rc, struct uwsgi_buffer *ub, struct uwsgi_buffer *ub2, char **rtp, size_t *rtp_len, uint8_t *channel) {
	char *buf = ub->buf;
	size_t len = ub->pos;

	// interleaved frame ?
	if (buf[0] == '$') {
		if (len < 4) return 0;
        	*channel = buf[1];
        	uint16_t pktsize = uwsgi_be16(buf + 2);
        	if (len < (size_t) (4 + pktsize)) return 0;
        	*rtp = buf + 4;
        	*rtp_len = pktsize;
		// invalid rtp packet
		if (*rtp_len < 12) return -1;
        	return 4 + pktsize;
	}

	// HTTp like ?
		ssize_t rlen = rtsp_find_rnrn(buf, len);
		if (rlen < 0) return -1;
		if (rlen == 0) return 0;
		char *method = NULL;
		size_t method_len = 0;
		size_t cl = 0;
		char *cseq = NULL;
		size_t cseq_len = 0;
		char *transport = NULL;
		size_t transport_len = 0;
		if (rtsp_parse(buf, rlen, &method, &method_len, &cl, &cseq, &cseq_len, &transport, &transport_len)) return -1;
		// need to read the body ?
		if (cl > 0) {
			if (len < rlen + cl) return 0;
		}

		if (!uwsgi_strncmp(method, method_len, "OPTIONS", 7)) {
			ub2->pos = 0;
			if (uwsgi_buffer_append(ub2, "RTSP/1.0 200 OK\r\n", 17)) return -1; 	
			if (cseq_len > 0) {
				if (uwsgi_buffer_append(ub2, "Cseq: ", 6)) return -1;
				if (uwsgi_buffer_append(ub2, cseq, cseq_len)) return -1;
				if (uwsgi_buffer_append(ub2, "\r\n", 2)) return -1;
			}
			if (uwsgi_buffer_append(ub2, "Public: DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, RECORD\r\n\r\n", 58)) return -1;
			return rlen + cl;
		}

		else if (!uwsgi_strncmp(method, method_len, "SETUP", 5)) {
                        ub2->pos = 0;
                        if (uwsgi_buffer_append(ub2, "RTSP/1.0 200 OK\r\n", 17)) return -1;
                        if (cseq_len > 0) {
                                if (uwsgi_buffer_append(ub2, "Cseq: ", 6)) return -1;
                                if (uwsgi_buffer_append(ub2, cseq, cseq_len)) return -1;
                                if (uwsgi_buffer_append(ub2, "\r\n", 2)) return -1;
                        }
                        if (transport_len > 0) {
                                if (uwsgi_buffer_append(ub2, "Transport: ", 11)) return -1;
                                if (uwsgi_buffer_append(ub2, transport, transport_len)) return -1;
                                if (uwsgi_buffer_append(ub2, "\r\n", 2)) return -1;
                        }
			if (uwsgi_buffer_append(ub2, "\r\n", 2)) return -1;
                        return rlen + cl;
                }

		// close the connection
		else if (!uwsgi_strncmp(method, method_len, "TEARDOWN", 8)) {
			return -1;
		}
		else if (!uwsgi_strncmp(method, method_len, "ANNOUNCE", 8)) {
			ub2->pos = 0;
                        if (uwsgi_buffer_append(ub2, "RTSP/1.0 200 OK\r\n", 17)) return -1;
                        if (cseq_len > 0) {
                                if (uwsgi_buffer_append(ub2, "Cseq: ", 6)) return -1;
                                if (uwsgi_buffer_append(ub2, cseq, cseq_len)) return -1;
                                if (uwsgi_buffer_append(ub2, "\r\n", 2)) return -1;
                        }
                        if (uwsgi_buffer_append(ub2, "\r\n", 2)) return -1;
			if (sdp_parse(rc, buf+rlen, cl)) return -1;
                        return rlen + cl;
		}
		else {
                        ub2->pos = 0;
                        if (uwsgi_buffer_append(ub2, "RTSP/1.0 200 OK\r\n", 17)) return -1;
                        if (cseq_len > 0) {
                                if (uwsgi_buffer_append(ub2, "Cseq: ", 6)) return -1;
                                if (uwsgi_buffer_append(ub2, cseq, cseq_len)) return -1;
                                if (uwsgi_buffer_append(ub2, "\r\n", 2)) return -1;
                        }
                        if (uwsgi_buffer_append(ub2, "\r\n", 2)) return -1;
                        return rlen + cl;
                }

		return 0;
}

int rtsp_router_func(struct wsgi_request *wsgi_req, struct uwsgi_route *ur) {
        if (!wsgi_req->socket->can_offload) {
                uwsgi_log("[realtime] unable to use \"rtsp\" router without offloading\n");
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
                        "publish", &rc->publish,
                        "video_demuxer", &rc->video_demuxer,
                        NULL)) {
                        uwsgi_log("[realtime] unable to parse stream action\n");
			goto end;
                }
        }
        else {
                rc->publish = uwsgi_str(ub->buf);
        }

	if (rc->video_demuxer) {
		if (!strcmp(rc->video_demuxer, "png")) {
			rc->video_rtp_demuxer = realtime_rtp_png;
		}
		else if (!strcmp(rc->video_demuxer, "vp8")) {
			rc->video_rtp_demuxer = realtime_rtp_vp8;
		}
		else if (!strcmp(rc->video_demuxer, "h264")) {
			rc->video_rtp_demuxer = realtime_rtp_h264;
		}
		else {
			uwsgi_log("[realtime] invalid video demuxer: %s\n", rc->video_demuxer);
                        goto end;
		}
	}

	uint16_t cseq_len = 0;
	char *cseq = uwsgi_get_var(wsgi_req, "HTTP_CSEQ", 9, &cseq_len); 

	// a request can start with OPTIONS or ANNOUNCE
	if (!uwsgi_strncmp(wsgi_req->method, wsgi_req->method_len, "OPTIONS", 7)) {
		if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
		if (uwsgi_response_add_header(wsgi_req, "Public", 6, "DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE, RECORD", 46)) goto end;
		if (cseq) {
			if (uwsgi_response_add_header(wsgi_req, "Cseq", 4, cseq, cseq_len)) goto end;
		} 
		if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
	}
	else if (!uwsgi_strncmp(wsgi_req->method, wsgi_req->method_len, "ANNOUNCE", 8)) {
		// a body is required for ANNOUNCE
		if (!wsgi_req->post_cl) goto end;
		if (consume_request_body(wsgi_req, rc)) goto end;
		if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
		if (cseq) {
			if (uwsgi_response_add_header(wsgi_req, "Cseq", 4, cseq, cseq_len)) goto end;
		} 
		if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
	} 
	else {
		goto end;
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

int rtsp_check(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor) {
	struct realtime_config *rc = (struct realtime_config *) uor->data;
	char *rtp = NULL;
        size_t rtp_len = 0;
        uint8_t channel = 0;
        ssize_t ret = rtsp_manage(rc, uor->ubuf, uor->ubuf1, &rtp, &rtp_len, &channel);
	if (ret <= 0) return ret;
                                        if (rtp) {
                                                if (channel == 0) {
                                                        if (rc->video_rtp_demuxer) {
                                                                int rtp_ret = rc->video_rtp_demuxer(rc, uor->ubuf3, rtp, rtp_len);
                                                                if (uwsgi_buffer_decapitate(uor->ubuf, ret)) return -1;
                                                                if (rtp_ret < 0) return -1;
								// more data ?
								if (rtp_ret == 0) return rtsp_check(ut, uor);
                                                                if (realtime_redis_build_publish(uor->ubuf1, uor->ubuf3->buf, uor->ubuf3->pos, rc)) return -1;
                                                        }
                                                        else {
                                                                if (realtime_redis_build_publish(uor->ubuf1, rtp, rtp_len, rc)) return -1;
                                                                if (uwsgi_buffer_decapitate(uor->ubuf, ret)) return -1;
                                                        }
                                                        uor->status = 3;
							uor->written = 0;	
                                                        if (event_queue_del_fd(ut->queue, uor->s, event_queue_read())) return -1;
                                                        if (event_queue_fd_read_to_write(ut->queue, uor->fd)) return -1;
                                                }
                                                else {
                                                        if (uwsgi_buffer_decapitate(uor->ubuf, ret)) return -1;
							// again
							return rtsp_check(ut, uor);
                                                }
                                        }
                                        else {
                                                if (uwsgi_buffer_decapitate(uor->ubuf, ret)) return -1;
                                                uor->status = 2;
						uor->written = 0;	
                                                if (event_queue_del_fd(ut->queue, uor->fd, event_queue_read())) return -1;
                                                if (event_queue_fd_read_to_write(ut->queue, uor->s)) return -1;
                                        }
                                        return 0;
}

/*

	RTSP offload engine

	0 -> wait for publish connection
	1 -> wait for HTTP/RTSP requests
	2 -> send HTTP/RTSP response
	3 -> publish
	

*/

int realtime_rtsp_offload_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {

        struct realtime_config *rc = (struct realtime_config *) uor->data;

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
			// HTTP/RTSP request
                        if (fd == uor->s) {
				if (uwsgi_buffer_ensure(uor->ubuf, rc->buffer_size)) return -1;		
                                ssize_t rlen = read(uor->s, uor->ubuf->buf + uor->ubuf->pos, rc->buffer_size);
				if (rlen == 0) return -1;
                                if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_rtsp_offload_do() -> read()");
                                        return -1;
                                }
				uor->ubuf->pos += rlen;
				return rtsp_check(ut, uor);
                        }

                        if (uor->fd == fd) {
                                // data from publish channel (consume, end on error)
                                if (uwsgi_buffer_ensure(uor->ubuf2, rc->buffer_size)) return -1;
                                ssize_t rlen = read(uor->fd, uor->ubuf2->buf + uor->ubuf2->pos, rc->buffer_size);
                                if (rlen == 0) return -1;
                                if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_rtsp_offload_do() -> read()");
                                        return -1;
                                }
                                uor->ubuf2->pos += rlen;
                                char array_type;
                                char *array;
                                int64_t array_len;
                                ssize_t ret = urt_redis_parse(uor->ubuf2->buf, uor->ubuf2->pos, &array_type, &array_len, &array);
                                if (ret > 0) {
                                        if (uwsgi_buffer_decapitate(uor->ubuf2, ret)) return -1;
                                        return 0;
                                }
                                return ret;
                        }
                        return -1;
                case 2:
                        // send response to client
                        if (fd == uor->s) {
                                ssize_t rlen = write(uor->s, uor->ubuf1->buf + uor->written, uor->ubuf1->pos-uor->written);
                                if (rlen > 0) {
                                        uor->written += rlen;
                                        if (uor->written >= (size_t)uor->ubuf1->pos) {
                                                // reset buffer
                                                uor->ubuf1->pos = 0;
                                                // back to wait
                                                uor->status = 1;
                                                if (event_queue_add_fd_read(ut->queue, uor->fd)) return -1;
                                                if (event_queue_fd_write_to_read(ut->queue, uor->s)) return -1;
                                        	return rtsp_check(ut, uor);
                                        }
					return 0;
                                }
                                else if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_rtsp_offload_do() -> write()");
                                }
                        }
                        return -1;
		case 3:
			// publish rtp packet
                        if (fd == uor->fd) {
                                ssize_t rlen = write(uor->fd, uor->ubuf1->buf + uor->written, uor->ubuf1->pos-uor->written);
                                if (rlen > 0) {
                                        uor->written += rlen;
                                        if (uor->written >= (size_t)uor->ubuf1->pos) {
                                                // reset buffer
                                                uor->ubuf1->pos = 0;
                                                // back to wait
                                                uor->status = 1;
                                                if (event_queue_add_fd_read(ut->queue, uor->s)) return -1;
                                                if (event_queue_fd_write_to_read(ut->queue, uor->fd)) return -1;
                                        	return rtsp_check(ut, uor);
                                        }
					return 0;
                                }
                                else if (rlen < 0) {
                                        uwsgi_offload_retry
                                        uwsgi_error("realtime_rtsp_offload_do() -> write()");
                                }
                        }
                        return -1;
                default:
                        return -1;
        }
        return -1;
}

