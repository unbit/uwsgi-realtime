#include "realtime.h"

/*

the following functions are various parsers for RTP packets.

They generally translates (and reassemble)from RTP to web-friendly format:

	RTP+PNG -> PNG
	RTP+VP8 -> webm_cluster+vp8 

*/


/*

	PNG decoder

	Every new TS is mapped to a new PNG image. A PNG image is full when the RTP marker is set

*/

static int realtime_rtp_png(struct realtime_config *rc, struct uwsgi_buffer *ub, char *rtp, size_t rtp_len) {
	uint8_t marker = (buf[1] >> 7) & 0x01;
	uint32_t ts = 0;
	memcpy(&ts, buf + 2, 4);
	ts = htonl(ts);

	// a new image is starting
	if (ts != rc->video_ts) {
		ub->pos = 0;
	}

	// append data;
	if (uwsgi_buffer_append(ub, rtp + 12, rtp_len - 12)) return -1;

	// if 1 we a have a full frame
	return marker;
}

static int realtime_rtp_vp8(struct realtime_config *rc, struct uwsgi_buffer *ub, char *rtp, size_t rtp_len) {
	return -1;
}
