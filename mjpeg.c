#include "realtime.h"

int mjpeg_router_func(struct wsgi_request *wsgi_req, struct uwsgi_route *ur) {
        if (!wsgi_req->socket->can_offload) {
                uwsgi_log("[realtime] unable to use \"mjpeg\" router without offloading\n");
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
                        "boundary", &rc->boundary,
                        NULL)) {
                        uwsgi_log("[realtime] unable to parse mjpeg action\n");
                        realtime_destroy_config(rc);
                        uwsgi_buffer_destroy(ub);
                        return UWSGI_ROUTE_BREAK;
                }
        }
        else {
                rc->subscribe = uwsgi_str(ub->buf);
        }

	if (!rc->boundary) {
		rc->boundary = uwsgi_malloc(37);
		uwsgi_uuid(rc->boundary);
	}

        if (!wsgi_req->headers_sent) {
                if (!wsgi_req->headers_size) {
                        if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
			char *boundary = uwsgi_concat2n("multipart/x-mixed-replace; boundary=", 36, rc->boundary, strlen(rc->boundary));
                        if (uwsgi_response_add_content_type(wsgi_req, boundary, strlen(boundary))) {free(boundary); goto end;}
			free(boundary);
			if (uwsgi_response_add_header(wsgi_req, "Cache-Control", 13, "no-cache", 8)) goto end;
			if (uwsgi_response_add_header(wsgi_req, "Cache-Control", 13, "private", 7)) goto end;
			if (uwsgi_response_add_header(wsgi_req, "Pragma", 6, "no-cache", 8)) goto end;
                }
                if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
        }

	char *body = uwsgi_concat3n("--", 2, rc->boundary, strlen(rc->boundary), "\r\n", 2);

        if (uwsgi_response_write_body_do(wsgi_req, body, strlen(body))) {
		free(body);
                goto end;
        }
	free(body);

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

int realtime_mjpeg_offload_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {

	struct realtime_config *rc = (struct realtime_config *) uor->data;

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
                        if (uwsgi_buffer_ensure(uor->ubuf, rc->buffer_size))
                                return -1;
                        ssize_t rlen = read(uor->fd, uor->ubuf->buf + uor->ubuf->pos, rc->buffer_size);
                        if (rlen > 0) {
                                uor->ubuf->pos += rlen;
                                // check if we have a full redis message
                                int64_t message_len = 0;
                                char *message;
                                ssize_t ret = urt_redis_pubsub(uor->ubuf->buf, uor->ubuf->pos, &message_len, &message);
                                if (ret > 0) {
                                        if (message_len > 0) {
						uor->ubuf1->pos	= 0;
						if (uwsgi_buffer_append(uor->ubuf1, "Content-Type: image/jpeg\r\nContent-Length: ", 42)) return -1;
						if (uwsgi_buffer_num64(uor->ubuf1, message_len)) return -1;
						if (uwsgi_buffer_append(uor->ubuf1, "\r\n\r\n", 4)) return -1;	
						if (uwsgi_buffer_append(uor->ubuf1, message, message_len)) return -1;
						if (uwsgi_buffer_append(uor->ubuf1, "\r\n--", 4)) return -1;
						if (uwsgi_buffer_append(uor->ubuf1, rc->boundary, rc->boundary_len)) return -1;
						if (uwsgi_buffer_append(uor->ubuf1, "\r\n", 2)) return -1;	
                                                uor->to_write = uor->ubuf1->pos;
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
                                uwsgi_offload_retry uwsgi_error("realtime_mjpeg_offload_do() -> read()/fd");
                        }
                }
                // an event from the client can only mean disconneciton
                return -1;
                // write event on s
        case 3:
                return realtime_write_ubuf(uor->ubuf1, ut, uor, 2);
        default:
                break;
        }

        return -1;
}
