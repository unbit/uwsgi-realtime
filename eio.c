#include "realtime.h"

extern struct uwsgi_server uwsgi;

// engine.io encoder
int eio_build(struct uwsgi_buffer *ub) {
        int ret = -1;
        uint64_t len = ub->pos;
        if (uwsgi_buffer_insert(ub, 0, "\0", 1)) return -1;
        size_t pos = 1;
        char *slen = uwsgi_64bit2str(len);
        char *ptr = slen;
        while(*ptr) {
                char num = *ptr - '0';
                if (uwsgi_buffer_insert(ub, pos, &num, 1)) goto error;
                ptr++;
                pos++;
        }
        char end = 0xff;
        if (uwsgi_buffer_insert(ub, pos, &end, 1)) goto error;
        ret = 0;
error:
        free(slen);
        return ret;
}

// assemble an engine.io response
int eio_build_http(struct uwsgi_buffer *ub) {
	if (eio_build(ub)) return -1;
	struct uwsgi_buffer *uhttp = uwsgi_buffer_new(uwsgi.page_size);
	if (uwsgi_buffer_append(uhttp, "HTTP/1.1 200 OK\r\n", 17)) goto end;
	if (uwsgi_buffer_append(uhttp, "Content-Type: application/octet-stream\r\n", 40)) goto end;
	if (uwsgi_buffer_append(uhttp, "Access-Control-Allow-Origin: *\r\n", 32)) goto end;
	if (uwsgi_buffer_append(uhttp, "Connection: Keep-Alive\r\n", 24)) goto end;
	if (uwsgi_buffer_append(uhttp, "Content-Length: ", 16)) goto end;
	if (uwsgi_buffer_num64(uhttp, ub->pos)) goto end;
	if (uwsgi_buffer_append(uhttp, "\r\n\r\n", 4)) goto end;

	if (uwsgi_buffer_insert(ub, 0, uhttp->buf, uhttp->pos)) goto end;
	uwsgi_buffer_destroy(uhttp);
	return 0;
end:
	uwsgi_buffer_destroy(uhttp);
	return -1;
}

int eio_parse_and_publish(struct uwsgi_buffer *ub, char *hb) {
	char *buf = ub->buf;
	size_t len = ub->pos;
	if (len == 0) return -1;
	if (*buf != 0) return -1;
	size_t pos = 0;
	// have we found a full header ?
	int found = 0;
	while(pos < sizeof(MAX64_STR)) {
		// need more data ?
		if (len < pos+1) return 0;
		if ((uint8_t) buf[pos+1] == 0xff) {
			found = 1;
			break;
		}
		// invalid data ?
		if (buf[pos+1] > 9) return -1;
		hb[pos] = buf[pos+1] + '0';
		pos++;
	}
	if (!found) return -1;

	uint64_t pktlen = uwsgi_n64(hb);

	// more data ?
	if (len < pos+2+pktlen) return 0;

	// TODO fix here
	struct realtime_config *rc = NULL;
	if (realtime_redis_publish(rc, buf + pos+2, pktlen)) return -1;

	// decapitate buffer

	if (uwsgi_buffer_decapitate(ub, pos+2+pktlen)) return -1;
	
	return 0;
}

// read the body of a POST engine.io request and publish to
// a redis channel
int eio_body_publish(struct wsgi_request *wsgi_req) {
	// we append each chunk to a uwsgi_buffer
	// that will be parsed at each cycle
	struct uwsgi_buffer *ub = uwsgi_buffer_new(uwsgi.page_size);
	// this memory area wil be used for parsing packet lengths
	char *hb = uwsgi_64bit2str(0);
	for(;;) {
		ssize_t rlen = 0;
		char *chunk = uwsgi_request_body_read(wsgi_req, 4096, &rlen);
		if (!chunk) goto error;
		if (chunk == uwsgi.empty) break;
		if (rlen <= 0) goto error;
		if (uwsgi_buffer_append(ub, chunk, rlen)) goto error;
		if (eio_parse_and_publish(ub, hb)) goto error;
	}
	uwsgi_buffer_destroy(ub);
	return 0;
error:
	uwsgi_buffer_destroy(ub);
	return -1;
}

int socketio_router_func(struct wsgi_request *wsgi_req, struct uwsgi_route *ur) {
        if (!uwsgi_strncmp(wsgi_req->method, wsgi_req->method_len, "POST", 4)) {
                // sid ?
                uint16_t sid_len = 0;
                char *sid = uwsgi_get_qs(wsgi_req, "sid", 3, &sid_len);
                if (!sid) return UWSGI_ROUTE_BREAK;
                if (!eio_body_publish(wsgi_req)) {
                        if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end0;
                        uwsgi_response_write_body_do(wsgi_req, "ok", 2);
                }
end0:
                return UWSGI_ROUTE_BREAK;
        }
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

	struct realtime_config *rc = uwsgi_calloc(sizeof(struct realtime_config));

        int mode = ur->custom ;

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
                                // the first output is not keepalive'd
                                if (uwsgi_response_add_header(wsgi_req, "Connection", 10, "close", 5)) goto end;
                        }
                        if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
                }
                // if no body has been sent, let's generate the json
                // {"sid":"SID","upgrades":["polling"],"pingTimeout":30000}
                if (wsgi_req->response_size == 0) {
                        struct uwsgi_buffer *ubody = uwsgi_buffer_new(uwsgi.page_size);
                        if (uwsgi_buffer_append(ubody, "0{\"sid\":\"", 9)) goto error;
                        if (uwsgi_buffer_append(ubody, ub->buf, ub->pos)) goto error;
#if UWSGI_PLUGIN_API > 1
                        if (uwsgi_buffer_append(ubody, "\",\"upgrades\":[\"websocket\"],\"pingInterval\":60000,\"pingTimeout\":90000}", 68)) goto error;
#else
                        if (uwsgi_buffer_append(ubody, "\",\"upgrades\":[],\"pingInterval\":60000,\"pingTimeout\":90000}", 57)) goto error;
#endif
                        if (eio_build(ubody)) goto error;
                        uwsgi_response_write_body_do(wsgi_req, ubody->buf, ubody->pos);
error:
                        uwsgi_buffer_destroy(ubody);
                }
                goto end;
        }

	uint16_t transport_len = 0;
        char *transport = uwsgi_get_qs(wsgi_req, "transport", 9, &transport_len);
        if (transport && !uwsgi_strncmp(transport, transport_len, "websocket", 9)) {
                if (uwsgi_websocket_handshake(wsgi_req, NULL, 0, NULL, 0, NULL, 0)) {
                        goto end;
                }
                mode = REALTIME_WEBSOCKET;
                goto offload;
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
	rc->engine = mode;
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
