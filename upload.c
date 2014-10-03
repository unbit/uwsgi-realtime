#include "realtime.h"

extern struct uwsgi_offload_engine *realtime_upload_offload_engine;

int realtime_upload_offload(struct wsgi_request *wsgi_req, int fd, off_t buf_size) {
        struct uwsgi_offload_request uor;
        uwsgi_offload_setup(realtime_upload_offload_engine, &uor, wsgi_req, 1);
	uor.buf = uwsgi_malloc(buf_size);
	uor.buf_pos = buf_size;
        return uwsgi_offload_run(wsgi_req, &uor, NULL);
}

int upload_router_func(struct wsgi_request *wsgi_req, struct uwsgi_route *ur) {
        if (!wsgi_req->socket->can_offload) {
                uwsgi_log("[realtime] unable to use \"upload\" router without offloading\n");
                return UWSGI_ROUTE_BREAK;
        }

        char **subject = (char **) (((char *)(wsgi_req))+ur->subject);
        uint16_t *subject_len = (uint16_t *)  (((char *)(wsgi_req))+ur->subject_len);

        struct uwsgi_buffer *ub = uwsgi_routing_translate(wsgi_req, ur, *subject, *subject_len, ur->data, ur->data_len);
        if (!ub) return UWSGI_ROUTE_BREAK;

	int fd = -1;
	off_t buf_size = 32768;

	// keyval ?
	if (!strchr(ub->buf, '=')) {
		fd = open(ub->buf, O_CREAT|O_TRUNC|O_WRONLY, 0664);
		if (fd < 0) {
			uwsgi_log("[realtime-upload] unable to open output file: %s\n", ub->buf);
			uwsgi_error("upload_router_func()/open()");
			goto error;
		}
	}
	else {
	}

        if (!realtime_upload_offload(wsgi_req, fd, buf_size)) {
                wsgi_req->via = UWSGI_VIA_OFFLOAD;
                wsgi_req->status = 202;
        }
error:
        uwsgi_buffer_destroy(ub);
        return UWSGI_ROUTE_BREAK;
}



