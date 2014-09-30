#include "realtime.h"

// 0 -> retry -> -1 error -> >0 -> size
ssize_t realtime_websocket_parse(struct uwsgi_buffer *ub, uint8_t *opcode, char **message, uint64_t *message_len) {
	char *buf = ub->buf;
	size_t len = ub->pos;

	if (len < 2) return 0; 

	uint8_t byte1 = buf[0];
	uint8_t byte2 = buf[1];

	*opcode = byte1 & 0xf;
	uint8_t has_mask = byte2 >> 7;
	uint64_t pktsize = byte2 & 0x7f; 
	uint64_t needed = 2;

	char *base = buf+2;

	// 16bit len
	if (pktsize == 126) {
		needed +=2;
		if (len < needed) return 0;
		pktsize = uwsgi_be16(base);
		base += 2;
	}
	// 64bit
	else if (pktsize == 127) {
		needed +=8;
		if (len < needed) return 0;
		pktsize = uwsgi_be64(base);
		base += 8;
	}

	*message_len = pktsize;
	*message = base;

	if (has_mask) {
		needed += 4;
		if (len < needed) return 0;
		char *mask = base;
		base += 4;
		*message = base;
		uint64_t i;
		for(i=0;i<pktsize;i++) {
			base[i] = base[i] ^ mask[i%4];
		}
	}

	return needed + pktsize;
}

int realtime_websocket_build(struct uwsgi_buffer *ub, int binary) {
	return -1;
}
