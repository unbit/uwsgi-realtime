#include "realtime.h"

extern struct uwsgi_server uwsgi;

/*

	this is pretty the same thing of the "strem" engine, but it will generate
	a valid webm header and expects webm clusters in the message dispatcher.

	WebM header:

		EBML (1a 45 df a3)
			EBML version (42 86) -> 1
			EBML read version (42 47) -> 1
			EBML max id length (42 fe) -> 4
			EBML max size length (42 f3) -> 8
			DOC type (42 82) "webm"
			DOC type version (42 87) 1
			DOC type read version (42 85) 1

		Segment (18 53 80 67) (unknown size, 01 ff ff ff ff ff ff ff)	
			Segment Information
			Segment tracks
				Track
					Video track
		Clusters follow ...

*/

int realtime_webm_8bit(struct uwsgi_buffer *ub, uint8_t n) {
	return uwsgi_buffer_u8(ub, n | 0x80);
}

int realtime_webm_64bit(struct uwsgi_buffer *ub, uint64_t n) {
	if (uwsgi_buffer_u64be(ub, n)) return -1;
	ub->buf[ub->pos-7] = 0x01;
	return 0;
}

static struct uwsgi_buffer *realtime_webm_begin(char *muxing_app, char *writing_app) {
	struct uwsgi_buffer *ub = uwsgi_buffer_new(uwsgi.page_size);
	if (uwsgi_buffer_append(ub, "\x1a\x45\xdf\xa3", 4)) goto error;
	// 31 bytes header
	if (realtime_webm_64bit(ub, 31)) goto error;
	// EBML version (42 86) -> 1
	if (uwsgi_buffer_append(ub, "\x42\x86\x81\x01", 4)) goto error;
	// EBML read version (42 47) -> 1
	if (uwsgi_buffer_append(ub, "\x42\x47\x81\x01", 4)) goto error;
	// EBML max id length (42 fe) -> 4
	if (uwsgi_buffer_append(ub, "\x42\xfe\x81\x04", 4)) goto error;
	// EBML max size length (42 f3) -> 8
	if (uwsgi_buffer_append(ub, "\x42\xf3\x81\x08", 4)) goto error;
	// DOC type (42 82) "webm"
	if (uwsgi_buffer_append(ub, "\x42\x82\x84webm", 7)) goto error;
	// DOC type version 
	if (uwsgi_buffer_append(ub, "\x42\x87\x81\x01", 4)) goto error;
	// DOC type read version (42 85) 1
	if (uwsgi_buffer_append(ub, "\x42\x85\x81\x01", 4)) goto error;

	// unlimited segment
	if (uwsgi_buffer_append(ub, "\x18\x53\x80\x67\x01\xff\xff\xff\xff\xff\xff\xff", 12)) goto error;

	// segment info

	// segment tracks
	
	return ub;
error:
	uwsgi_buffer_destroy(ub);
	return NULL;	
}

static int realtime_webm_track_video(struct uwsgi_buffer *ub, uint8_t id, char *codec, uint8_t fps, uint16_t width, uint16_t height) {
	return 0;
}

int webm_router_func(struct wsgi_request *wsgi_req, struct uwsgi_route *ur) {
        if (!wsgi_req->socket->can_offload) {
                uwsgi_log("[realtime] unable to use \"webm\" router without offloading\n");
                return UWSGI_ROUTE_BREAK;
        }

        char **subject = (char **) (((char *)(wsgi_req))+ur->subject);
        uint16_t *subject_len = (uint16_t *)  (((char *)(wsgi_req))+ur->subject_len);

        struct uwsgi_buffer *ub = uwsgi_routing_translate(wsgi_req, ur, *subject, *subject_len, ur->data, ur->data_len);
        if (!ub) return UWSGI_ROUTE_BREAK;

        if (!wsgi_req->headers_sent) {
                if (!wsgi_req->headers_size) {
                        if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
			if (uwsgi_response_add_content_type(wsgi_req, "video/webm", 10)) goto end;
                }
                if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
        }

	struct uwsgi_buffer *webm = realtime_webm_begin("uWSGI", "uWSGI");
	if (!webm) goto end;

	if (realtime_webm_track_video(webm, 0, "V_VP8", 30, 320, 240)) {
		uwsgi_buffer_destroy(webm);
		goto end;
	}

	if (uwsgi_response_write_body_do(wsgi_req, webm->buf, webm->pos)) {
		uwsgi_buffer_destroy(webm);
		goto end;
	}

	uwsgi_buffer_destroy(webm);

        if (!realtime_redis_offload(wsgi_req, ub->buf, ub->pos, ur->custom)) {
                wsgi_req->via = UWSGI_VIA_OFFLOAD;
                wsgi_req->status = 202;
        }
end:
        uwsgi_buffer_destroy(ub);
        return UWSGI_ROUTE_BREAK;
}
