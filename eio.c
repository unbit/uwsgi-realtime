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
