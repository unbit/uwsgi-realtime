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

/*
	- offload engine for socket.io websocket

	options -> subscribe=<subscribe_channel>,publish=<publish_channel>,sid=<sid>,prefix=<prfix_to_add_to_each_message>

	fd -> connection for subscription
	fd2 -> connection for publish 
	s -> websocket connection

	status 0 -> waiting for fd to be opened
	status 1 -> subscribe to fd
	status 2 -> connect to fd2
	status 3 -> waiting data on s,fd,fd2

	status 4 -> writing to s
	status 5 -> publishing to fd2
*/
int realtime_websocket_offload_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {
	// setup
        if (fd == -1) {
                event_queue_add_fd_write(ut->queue, uor->fd);
                return 0;
        }

        switch(uor->status) {
                // waiting for fd connection
                case 0:
                        if (fd == uor->fd) {
                                uor->status = 1;
                                // ok try to send the request right now...
                                return realtime_redis_offload_engine_do(ut, uor, fd);
                        }
                        return -1;
		// on full write, connect to fd2
		case 1:
			break;
		case 2:
			// connected to fd2, now start waiting for data
			break;
		case 3:
			break;
		case 4:
			// write received message to the socket
			break;
		case 5:
			// publish message to redis
			break;
		default:
			return -1;
	}
	return -1;
}
