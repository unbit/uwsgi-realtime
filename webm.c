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

int realtime_webm_64bit(struct uwsgi_buffer *ub, uint64_t n) {
	if (uwsgi_buffer_u64be(ub, n)) return -1;
	ub->buf[ub->pos-8] = 0x01;
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
	if (uwsgi_buffer_append(ub, "\x42\xf7\x81\x01", 4)) goto error;
	// EBML max id length (42 fe) -> 4
	if (uwsgi_buffer_append(ub, "\x42\xf2\x81\x04", 4)) goto error;
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
	if (uwsgi_buffer_append(ub, "\x15\x49\xa9\x66", 4)) goto error;
	// leave space for final size
	size_t segment_info_pos = ub->pos;
	ub->pos+=8;
	// TimecodeScale 1000000 -> 0x0f4240
	if (uwsgi_buffer_append(ub, "\x2a\xd7\xb1\x83\x0f\x42\x40", 7)) goto error;
	// MuxingApp
	if (uwsgi_buffer_append(ub, "\x4d\x80", 2)) goto error;
	if (realtime_webm_64bit(ub, strlen(muxing_app))) goto error;
	if (uwsgi_buffer_append(ub, muxing_app, strlen(muxing_app))) goto error;
	// WritingApp
	if (uwsgi_buffer_append(ub, "\x57\x41", 2)) goto error;
	if (realtime_webm_64bit(ub, strlen(writing_app))) goto error;
	if (uwsgi_buffer_append(ub, writing_app, strlen(writing_app))) goto error;
	// SegmentUID
	if (uwsgi_buffer_append(ub, "\x73\xa4\x90", 3)) goto error;
	if (uwsgi_buffer_append(ub, "\1\2\3\4\5\6\7\x08\x09\0\1\2\3\4\5\6", 16)) goto error;

	// fix buffer
	size_t current_pos = ub->pos;
	ub->pos = segment_info_pos;
	if (realtime_webm_64bit(ub, (current_pos - segment_info_pos)-8)) goto error;
	ub->pos = current_pos;
	
	// segment tracks follow ...
	if (uwsgi_buffer_append(ub, "\x16\x54\xae\x6b", 4)) goto error;
	
	return ub;
error:
	uwsgi_buffer_destroy(ub);
	return NULL;	
}

static int realtime_webm_track_video(struct uwsgi_buffer *ub, uint8_t id, char *codec, uint8_t fps, uint16_t width, uint16_t height) {
	// avoid division by zero
	if (!fps) fps = 30;

	if (uwsgi_buffer_u8(ub, 0xAE)) return -1;
	size_t track_pos = ub->pos;
	// leave space for size
	ub->pos+=8;
	// TrackNumber
	if (uwsgi_buffer_append(ub, "\xd7\x81", 2)) return -1;
	if (uwsgi_buffer_u8(ub, id)) return -1;
	// TrackUID
	if (uwsgi_buffer_append(ub, "\x73\xc5\x81", 3)) return -1;
	if (uwsgi_buffer_u8(ub, id)) return -1;
	// FlagLacing
	if (uwsgi_buffer_append(ub, "\x9c\x81\x00", 3)) return -1;
	// Language
	if (uwsgi_buffer_append(ub, "\x22\xb5\x9c\x83und", 7)) return -1;
	// CodecID
	if (uwsgi_buffer_u8(ub, 0x86)) return -1;
	if (realtime_webm_64bit(ub, strlen(codec))) return -1;
	if (uwsgi_buffer_append(ub, codec, strlen(codec))) return -1;
	// TrackType
	if (uwsgi_buffer_append(ub, "\x83\x81\x01", 3)) return -1;
	// DefaultDuration
	if (uwsgi_buffer_append(ub, "\x23\xe3\x83\x84", 4)) return -1;
	if (uwsgi_buffer_u32be(ub, (1000 * 1000 * 1000) / fps)) return -1;
	// Video
	if (uwsgi_buffer_append(ub, "\xE0\x88", 2)) return -1;
	// Width
	if (uwsgi_buffer_append(ub, "\xb0\x82", 2)) return -1;
	if (uwsgi_buffer_u16be(ub, width)) return -1;
	// Height
	if (uwsgi_buffer_append(ub, "\xba\x82", 2)) return -1;
	if (uwsgi_buffer_u16be(ub, height)) return -1;

	// fix track size
	size_t current_pos = ub->pos;
	ub->pos = track_pos;
	if (realtime_webm_64bit(ub, (current_pos - track_pos)-8)) return -1;
	ub->pos = current_pos;
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

	struct realtime_config *rc = uwsgi_calloc(sizeof(struct realtime_config));

	if (strchr(ub->buf, '=')) {
                if (uwsgi_kvlist_parse(ub->buf, ub->pos, ',', '=',
                        "server", &rc->server,
                        "subscribe", &rc->subscribe,
                        "video_codec", &rc->video_codec,
                        NULL)) {
                        uwsgi_log("[realtime] unable to parse webm action\n");
                        realtime_destroy_config(rc);
                        uwsgi_buffer_destroy(ub);
                        return UWSGI_ROUTE_BREAK;
                }
        }
        else {
                rc->subscribe = uwsgi_str(ub->buf);
		rc->video_codec = uwsgi_str("V_VP8");	
        }

        if (!wsgi_req->headers_sent) {
                if (!wsgi_req->headers_size) {
                        if (uwsgi_response_prepare_headers(wsgi_req, "200 OK", 6)) goto end;
			if (uwsgi_response_add_content_type(wsgi_req, "video/webm", 10)) goto end;
                }
                if (uwsgi_response_write_headers_do(wsgi_req) < 0) goto end;
        }

	struct uwsgi_buffer *webm = realtime_webm_begin("uWSGI", "uWSGI");
	if (!webm) goto end;

	size_t tracks_pos = webm->pos;
	// leave space for tracks size
	webm->pos += 8;

	if (realtime_webm_track_video(webm, 1, rc->video_codec, 30, 320, 240)) {
		uwsgi_buffer_destroy(webm);
		goto end;
	}

	// now fix the tracks size
	size_t current_pos = webm->pos;
	webm->pos = tracks_pos;
        if (realtime_webm_64bit(webm, (current_pos - tracks_pos)-8)) goto end;
        webm->pos = current_pos;

	if (uwsgi_response_write_body_do(wsgi_req, webm->buf, webm->pos)) {
		uwsgi_buffer_destroy(webm);
		goto end;
	}

	uwsgi_buffer_destroy(webm);

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

int realtime_webm_cluster(struct realtime_config *rc, struct uwsgi_buffer *ub, char *buf, size_t len) {
	if (uwsgi_buffer_append(ub, "\x1F\x43\xB6\x75", 4)) return -1;
	size_t cluster_pos = ub->pos;
	ub->pos += 8;
	// CLUSTER
	if (uwsgi_buffer_append(ub, "\xE7\x84", 2)) return -1;
	// CLUSTER timecode
	//if (uwsgi_buffer_u32be(ub, rc->video_last_ts - rc->start_ts)) return -1;
	if (uwsgi_buffer_u32be(ub, rc->ts)) return -1;
	rc->ts += 33;
	// simpleblock
	if (uwsgi_buffer_append(ub, "\xA3", 1)) return -1;
	if (realtime_webm_64bit(ub, len + 4)) return -1;
	// keyframe
	if (uwsgi_buffer_append(ub, "\x81\x00\x00\x80", 4)) return -1;
	// payload
	if (uwsgi_buffer_append(ub, buf, len)) return -1;
	size_t current_pos = ub->pos;
	ub->pos = cluster_pos;	
	if (realtime_webm_64bit(ub, (current_pos - cluster_pos)-8)) return -1;
	ub->pos = current_pos;
	return 0;
}

int realtime_webm_offload_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {
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
                                                uor->written = 0;
						// is it a CLUSTER tag ?
						if (message_len > 4 && (uint8_t) message[0] == 0x1F &&
									(uint8_t) message[1] == 0x43 &&		
									(uint8_t) message[2] == 0xB6 &&		
									(uint8_t) message[3] == 0x75) {
							if (uwsgi_buffer_append(uor->ubuf1, message, message_len)) return -1;	
						}
						// ... if not, mux it
						else {
                                                	if (realtime_webm_cluster(rc, uor->ubuf1, message, message_len)) return -1;
						}
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
                                uwsgi_offload_retry uwsgi_error("realtime_redis_offload_engine_do() -> read()/fd");
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
