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

	if (realtime_redis_publish(buf + pos+2, pktlen, "uwsgi", 5)) return -1;

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
