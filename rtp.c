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
	uint32_t ts = rc->video_last_ts;
	rc->video_last_ts = uwsgi_be32(rtp+4);

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
	uint32_t ts = rc->video_last_ts;
        rc->video_last_ts = uwsgi_be32(rtp+4);

        size_t header_size = 12;

        uint8_t cc = rtp[0] & 0x0f;
        header_size += cc * 4;

	if (header_size >= rtp_len) return -1;

	char *buf = rtp + header_size;
	size_t len = rtp_len - header_size;

	if (len < 1) return -1;

	header_size++;

	uint8_t extended = (buf[0] >> 7 ) & 0x01;
	uint8_t has_pictureid = 0;
	uint8_t has_tl0 = 0;
	uint8_t has_tid = 0;
	uint8_t has_keyidx = 0;

	if (extended && len < 2) return -1;
	//uint8_t start_of_partition = (buf[0] >> 4 ) & 0x01;
	//uint8_t pid = buf[0] & 0x7;
	if (extended) {
		header_size++;
		has_pictureid = (buf[1] >> 7) & 0x01;
		has_tl0 = buf[1] & 0x40;
		has_tid = buf[1] & 0x20;
		has_keyidx = buf[1] & 0x10;
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

	//uwsgi_log("[%u %u] marker = %u extended = %d pictureid = %u/%u start_of_partition %d partition %d header_size %d\n", rtp_len, rc->video_last_ts, marker, extended, pictureid, m, start_of_partition, pid, header_size);

	/*
	if (start_of_partition && pid == 0) {
		ub->pos = 0;
	}
	*/

	if (ts != rc->video_last_ts) {
                ub->pos = 0;
        }

	if (uwsgi_buffer_append(ub, rtp + header_size, rtp_len - header_size)) return -1;

	return marker;
}

int realtime_rtp_h264(struct realtime_config *rc, struct uwsgi_buffer *ub, char *rtp, size_t rtp_len) {
        uint8_t marker = (rtp[1] >> 7) & 0x01;
        uint8_t padding = (rtp[0] >> 5) & 0x01;
        if (padding) rtp_len--;

	uint32_t ts = rc->video_last_ts;
        rc->video_last_ts = uwsgi_be32(rtp+4);

	uwsgi_log("[H264] ts = %u\n", rc->video_last_ts);

        size_t header_size = 12;

        uint8_t cc = rtp[0] & 0x0f;
        header_size += cc * 4;

        if (header_size >= rtp_len) return -1;
	char *buf = rtp + header_size;
        size_t len = rtp_len - header_size;

        if (len < 1) return -1;

	//uwsgi_log("[AVC %u %u] type = %X %X type0=%u type1=%u marker=%u (size: %u)\n", rc->video_last_ts, uwsgi_be16(rtp+2),  buf[0], buf[1], buf[0] & 0x1f, buf[1] & 0x1f, marker, rtp_len);

	uint8_t nal_type = buf[0] & 0x1f;
	uint8_t nal_base = buf[0] & 0xe0;
	if (nal_type <= 23) {
		if (rc->sprop) {
			if (uwsgi_buffer_append(ub, rc->sprop->buf, rc->sprop->pos)) return -1;
		}
		if (rc->video_last_ts != ts) {
			ub->pos = 0;
			// append start code
		}
		if (uwsgi_buffer_append(ub, "\0\0\0\1", 4)) return -1;
		if (uwsgi_buffer_append(ub, buf, len)) return -1;
		//marker = 1;
	}
	// STAP-A
	else if (nal_type == 24) {
		if (rc->video_last_ts != ts) {
			ub->pos = 0;
		}
		buf++;
		len--;		
		while(len > 2) {
			// read nal size
			uint16_t nal_size = uwsgi_be16(buf);
			buf += 2;
			len -= 2;
			// invalid packet ?
			if (len < nal_size) return -1;
			if (rc->sprop) {
				if (uwsgi_buffer_append(ub, rc->sprop->buf, rc->sprop->pos)) return -1;
			}
			if (uwsgi_buffer_append(ub, "\0\0\0\1", 4)) return -1;
			//uwsgi_log("NAL type = %u\n", buf[0] & 0x1f);
			if (uwsgi_buffer_append(ub, buf, nal_size)) return -1;
			buf += nal_size;
			len -= nal_size;
		}
		//marker = 1;
	}
	// fragmented packet ?
	else if (nal_type == 28 ) {
		if (len < 2) return -1;
		if (rc->video_last_ts != ts) {
			ub->pos = 0;
		}
		// true type
		nal_type = buf[1] & 0x1f;
		//uint8_t end_bit = (buf[1] >> 6) & 0x01;
		// is it the first packet ?
		if (buf[1] & 0x80) {
			//ub->pos = 0;
			if (rc->sprop) {
				if (uwsgi_buffer_append(ub, rc->sprop->buf, rc->sprop->pos)) return -1;
			}
			// append start code
			if (uwsgi_buffer_append(ub, "\0\0\0\1", 4)) return -1;
			// append original nal
			if (uwsgi_buffer_u8(ub, nal_base | nal_type)) return -1;
		}
		buf += 2;
		len -= 2;
		//marker = end_bit;
		if (uwsgi_buffer_append(ub, buf, len)) return -1;
	}
	// other kind of NALs are skipped ...

	return marker;
}


int realtime_rtp_aac(struct realtime_config *rc, struct uwsgi_buffer *ub, char *rtp, size_t rtp_len) {
        uint8_t marker = (rtp[1] >> 7) & 0x01;
        uint8_t padding = (rtp[0] >> 5) & 0x01;
        if (padding) rtp_len--;
        uint32_t ts = rc->audio_last_ts;
        rc->audio_last_ts = uwsgi_be32(rtp+4);

	uwsgi_log("[AAC] ts = %u\n", rc->audio_last_ts);

        size_t header_size = 12;

        uint8_t cc = rtp[0] & 0x0f;
        header_size += cc * 4;

	if (header_size >= rtp_len) return -1;

        char *buf = rtp + header_size;
        size_t len = rtp_len - header_size;

	if (len < 2) return -1;

	if (rc->indexlength+rc->sizelength == 0) return -1;

	uint16_t au_bits = uwsgi_be16(buf);
	uint16_t au_bytes = (au_bits + 7) / 8;

	buf += 2;
	len -= 2;

	if (len < au_bytes) return -1;

	if (au_bits % (rc->indexlength+rc->sizelength) > 0) return -1;

	uint16_t au_n = au_bits / (rc->indexlength+rc->sizelength);

	if (ts != rc->audio_last_ts) {
                ub->pos = 0;
        }

	uint16_t i;
	char *base = buf;
	size_t remains = len - au_bytes;
	char *pkt = buf + au_bytes;
	for(i=0;i<au_n;i++) {
		uint16_t au_size = uwsgi_be16(base);
		au_size = au_size >> (16 - rc->sizelength);
		if (au_size <= remains) {
			uint8_t profile = 15;
			uint8_t freq = 4;
			uint8_t chan = 2;
			uint8_t adts[7];
			uint16_t pktlen = au_size + 7;
			adts[0] = 0xff;
			adts[1] = 0xf1;
			// 2 bits profile + 4 bits freq + 1 bit private (0) + 1 bit first part of 3 bit channel
			adts[2] = ((profile-1) << 6) + (freq << 2) + (chan >> 2);
			// 2 bits channel (second part)  
			adts[3] = ((chan & 0x03) << 6) + (pktlen>>11);
			adts[4] = (pktlen & 0x7FF) >> 3;
			adts[5] = ((pktlen&7)<<5) + 0x1F;
			adts[6] = 0xFC;
			if (uwsgi_buffer_append(ub, (char *)adts, 7)) return -1;
			if (uwsgi_buffer_append(ub, pkt, au_size)) return -1;
			pkt += au_size;
			remains -= au_size;
			base += ((rc->indexlength+rc->sizelength) +7) / 8;
			continue;
		}
		return -1;
	}

	return marker;
}
