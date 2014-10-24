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
	uint8_t padding = (rtp[0] >> 5) & 0x01;
	if (padding) rtp_len--;
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
	uint8_t padding = (rtp[0] >> 5) & 0x01;
	if (padding) rtp_len--;
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
	uint8_t has_pictureid = 0;
	uint8_t has_tl0 = 0;
	uint8_t has_tid = 0;
	uint8_t has_keyidx = 0;

	if (extended && len < 2) return -1;
	uint8_t start_of_partition = (buf[0] >> 4 ) & 0x01;
	uint8_t pid = buf[0] & 0x7;
	if (extended) {
		header_size++;
		has_pictureid = (buf[1] >> 7) & 0x01;
		has_tl0 = (buf[1] >> 6) & 0x1f;
		has_tid = (buf[1] >> 5) & 0xf;
		has_keyidx = (buf[1] >> 4) & 0x7;
	}
	if (has_pictureid && len < 3) return -1;
	uint16_t pictureid = 0;
	uint8_t m = 0;
	if (has_pictureid) {
		header_size++;
		m = (buf[2] >> 7) & 0x01;
		if (m) {
			header_size++;
			buf[2] = buf[2] & 0x7f;
			memcpy(&pictureid, buf + 2, 2);
			pictureid = ntohs(pictureid);
			
		}
		else {
			pictureid = buf[2] & 0x7f;
		}
		
	}

	if (has_tl0) header_size++;
	if (has_tid || has_keyidx) header_size++;

	uwsgi_log("[%u %u] marker = %u extended = %d pictureid = %u/%u start_of_partition %d partition %d header_size %d\n", rtp_len, ts, marker, extended, pictureid, m, start_of_partition, pid, header_size);

	if (start_of_partition && pid == 0) {
		ub->pos = 0;
	}

	uint64_t hash = 0;
	size_t i;
	for(i=header_size;i<rtp_len - header_size;i++) {
		hash += rtp[i];
	}

	if (uwsgi_buffer_append(ub, rtp + header_size, rtp_len - header_size)) return -1;

	uwsgi_log("len = %d pos = %d hash %llu \n", ub->len, ub->pos, hash);
	return marker;
}
