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

int realtime_rtp_png(struct realtime_config *rc, struct uwsgi_buffer *ub, char *rtp, size_t rtp_len) {
	uint8_t marker = (rtp[1] >> 7) & 0x01;
	uint32_t ts = 0;
	memcpy(&ts, rtp + 2, 4);
	ts = htonl(ts);

	size_t header_size = 12;

	uint8_t cc = rtp[0] & 0x0f;
	header_size += cc * 4;

	// a new image is starting
	if (ts != rc->video_last_ts) {
		ub->pos = 0;
	}

	rc->video_last_ts = ts;

	// append data;
	if (uwsgi_buffer_append(ub, rtp + header_size, rtp_len - header_size)) return -1;

	// if 1 we a have a full frame
	return marker;
}

int realtime_rtp_vp8(struct realtime_config *rc, struct uwsgi_buffer *ub, char *rtp, size_t rtp_len) {
	uint8_t marker = (rtp[1] >> 7) & 0x01;
        uint32_t ts = 0;
        memcpy(&ts, rtp + 2, 4);
        ts = htonl(ts);

        size_t header_size = 12;

        uint8_t cc = rtp[0] & 0x0f;
        header_size += cc * 4;

	if (header_size >= rtp_len) return -1;

	char *buf = rtp + header_size;
	size_t len = rtp_len - header_size;

	uint8_t extended = (buf[0] >> 7 ) & 0x01;
	if (extended && len < 2) return -1;
	uint8_t start_of_partition = (buf[0] >> 4 ) & 0x01;
	uint8_t pid = buf[0] & 0x7;
	uwsgi_log("[%u %u] marker = %u extended = %d start_of_partition %d partition %d\n", rtp_len, ts, marker, extended, start_of_partition, pid);
	return marker;
}
