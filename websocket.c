#include "realtime.h"

// 0 -> retry -> -1 error -> >0 -> size
ssize_t realtime_websocket_parse(struct uwsgi_buffer *ub, uint8_t * opcode, char **message, uint64_t * message_len) {
	char *buf = ub->buf;
	size_t len = ub->pos;

	if (len < 2)
		return 0;

	uint8_t byte1 = buf[0];
	uint8_t byte2 = buf[1];

	*opcode = byte1 & 0xf;
	uint8_t has_mask = byte2 >> 7;
	uint64_t pktsize = byte2 & 0x7f;
	uint64_t needed = 2;

	char *base = buf + 2;

	// 16bit len
	if (pktsize == 126) {
		needed += 2;
		if (len < needed)
			return 0;
		pktsize = uwsgi_be16(base);
		base += 2;
	}
	// 64bit
	else if (pktsize == 127) {
		needed += 8;
		if (len < needed)
			return 0;
		pktsize = uwsgi_be64(base);
		base += 8;
	}

	*message_len = pktsize;
	*message = base;

	if (has_mask) {
		needed += 4;
		if (len < needed)
			return 0;
		char *mask = base;
		base += 4;
		*message = base;
		uint64_t i;
		for (i = 0; i < pktsize; i++) {
			base[i] = base[i] ^ mask[i % 4];
		}
	}

	return needed + pktsize;
}

int realtime_websocket_build(struct uwsgi_buffer *ub, uint8_t opcode, char *msg, uint64_t len) {
	if (uwsgi_buffer_u8(ub, 0x80 | opcode))
		return -1;
	if (len < 126) {
		if (uwsgi_buffer_u8(ub, len))
			return -1;
	}
	else if (len <= (uint16_t) 0xffff) {
		if (uwsgi_buffer_u8(ub, 126))
			return -1;
		if (uwsgi_buffer_u16be(ub, len))
			return -1;
	}
	else {
		if (uwsgi_buffer_u8(ub, 127))
			return -1;
		if (uwsgi_buffer_u64be(ub, len))
			return -1;
	}

	if (uwsgi_buffer_append(ub, msg, len))
		return -1;
	return 0;
}

/*
	- offload engine for socket.io websocket

	options -> subscribe=<subscribe_channel>,publish=<publish_channel>,sid=<sid>,prefix=<prefix_to_add_to_each_message>,binary=0/1

	fd -> connection for subscription
	fd2 -> connection for publish 
	s -> websocket connection

	status 0 -> waiting for fd to be opened
	status 1 -> subscribe to fd
	status 2 -> connect to fd2
	status 3 -> wait for fd2 connection
	status 4 -> waiting data on s,fd,fd2

	status 4 -> writing to s
	status 5 -> publishing to fd2
*/
int realtime_websocket_offload_do(struct uwsgi_thread *ut, struct uwsgi_offload_request *uor, int fd) {

	struct realtime_config *rc = (struct realtime_config *) uor->data;

	uwsgi_log("ENTERING WEBSOCKET %d\n", uor->status);

	switch (uor->status) {
		// waiting for fd connection
	case 0:
		if (fd == uor->fd) {
			uor->status = 1;
			// ok try to send the request right now...
			return realtime_websocket_offload_do(ut, uor, fd);
		}
		return -1;
		// on full write, connect to fd2
	case 1:
		if (fd == uor->fd) {
			ssize_t rlen = write(uor->fd, uor->ubuf->buf + uor->written, uor->ubuf->pos - uor->written);
			uwsgi_log("written %d\n", rlen);
			if (rlen > 0) {
				uor->written += rlen;
				if (uor->written >= (size_t) uor->ubuf->pos) {
					// reset buffer
					uor->ubuf->pos = 0;
					uor->status = 2;
					if (event_queue_del_fd(ut->queue, uor->fd, event_queue_write()))
						return -1;
					return realtime_websocket_offload_do(ut, uor, fd);
				}
				return 0;
			}
			else if (rlen < 0) {
				uwsgi_offload_retry uwsgi_error("realtime_websocket_offload_do() -> write()");
			}
		}
		return -1;
	case 2:
		// connected to fd2, now start waiting for data
		uor->fd2 = uwsgi_connect(uor->name, 0, 1);
		if (uor->fd2 < 0)
			return -1;
		uor->status = 3;
		event_queue_add_fd_write(ut->queue, uor->fd2);
		return 0;
	case 3:
		if (uor->fd2 == fd) {
			if (event_queue_add_fd_read(ut->queue, uor->s))
				return -1;
			if (event_queue_add_fd_read(ut->queue, uor->fd))
				return -1;
			if (event_queue_fd_write_to_read(ut->queue, uor->fd2))
				return -1;
			uor->status = 4;
			return 0;
		}
		return -1;
	case 4:
		// data from socket (read into ubuf1, write to ubuf)
		uwsgi_log("DATA from socket\n");
		if (uor->s == fd) {
			if (uwsgi_buffer_ensure(uor->ubuf1, 4096))
				return -1;
			ssize_t rlen = read(uor->s, uor->ubuf1->buf + uor->ubuf1->pos, 4096);
			uwsgi_log("rlen =%d\n", rlen);
			if (rlen == 0)
				return -1;
			if (rlen < 0) {
				uwsgi_offload_retry uwsgi_error("realtime_websocket_offload_do() -> read()");
				return -1;
			}
			uor->ubuf1->pos += rlen;
			char *message = NULL;
			uint64_t message_len = 0;
			uint8_t opcode = 0;
			ssize_t ret = realtime_websocket_parse(uor->ubuf1, &opcode, &message, &message_len);
			if (ret > 0) {
				uwsgi_log("websocket message of %d bytes (%d %.*s)!!!\n", rlen, opcode, message_len, message);
				// reset buffer
				uor->ubuf->pos = 0;
				if (realtime_redis_build_publish(uor->ubuf, message, message_len, rc->publish, rc->publish_len))
					return -1;
				if (uwsgi_buffer_decapitate(uor->ubuf1, rlen))
					return -1;
				// now publish the message to redis
				uor->written = 0;
				uor->status = 6;
				if (event_queue_del_fd(ut->queue, uor->s, event_queue_read()))
					return -1;
				if (event_queue_del_fd(ut->queue, uor->fd, event_queue_read()))
					return -1;
				if (event_queue_fd_read_to_write(ut->queue, uor->fd2))
					return -1;
				return 0;
			}
			return ret;
		}
		// data from redis (read into ubuf2, write to ubuf)
		if (uor->fd == fd) {
			uwsgi_log("DATA from redis\n");
			if (uwsgi_buffer_ensure(uor->ubuf2, 4096))
				return -1;
			ssize_t rlen = read(uor->fd, uor->ubuf2->buf + uor->ubuf2->pos, 4096);
			if (rlen == 0)
				return -1;
			if (rlen < 0) {
				uwsgi_offload_retry uwsgi_error("realtime_websocket_offload_do() -> read()");
				return -1;
			}
			uor->ubuf2->pos += rlen;
			char *message = NULL;
			int64_t message_len = 0;
			ssize_t ret = urt_redis_pubsub(uor->ubuf2->buf, uor->ubuf2->pos, &message_len, &message);
			if (ret > 0) {
				uwsgi_log("redis message |%.*s|\n", message_len, message);
				if (message_len > 0) {
					// reset buffer
					uor->ubuf->pos = 0;
					if (realtime_websocket_build(uor->ubuf, 1, message, message_len))
						return -1;
					// now publish the message to redis
					uor->written = 0;
					uor->status = 5;
					if (event_queue_del_fd(ut->queue, uor->fd2, event_queue_read()))
						return -1;
					if (event_queue_del_fd(ut->queue, uor->fd, event_queue_read()))
						return -1;
					if (event_queue_fd_read_to_write(ut->queue, uor->s))
						return -1;
				}
				if (uwsgi_buffer_decapitate(uor->ubuf2, ret))
					return -1;
				return 0;
			}
			return ret;
		}
		// data from publish channel (consume, end on error)
		if (uor->fd2 == fd) {
			uwsgi_log("DATA FROM PUBLISH\n");
			if (uwsgi_buffer_ensure(uor->ubuf3, 4096))
				return -1;
			ssize_t rlen = read(uor->fd2, uor->ubuf3->buf + uor->ubuf3->pos, 4096);
			if (rlen == 0)
				return -1;
			if (rlen < 0) {
				uwsgi_offload_retry uwsgi_error("realtime_websocket_offload_do() -> read()");
				return -1;
			}
			uor->ubuf3->pos += rlen;
			char *message = NULL;
			int64_t message_len = 0;
			ssize_t ret = urt_redis_pubsub(uor->ubuf2->buf, uor->ubuf2->pos, &message_len, &message);
			if (ret > 0) {
				if (uwsgi_buffer_decapitate(uor->ubuf2, ret))
					return -1;
				return 0;
			}
			return ret;
		}
		return -1;
	case 5:
		// write received message to the socket
		if (fd == uor->s) {
			uwsgi_log("READY TO WRITe %.*s\n", uor->ubuf->pos - uor->written, uor->ubuf->buf + uor->written);
			ssize_t rlen = write(uor->s, uor->ubuf->buf + uor->written, uor->ubuf->pos - uor->written);
			uwsgi_log("written %d\n", rlen);
			if (rlen > 0) {
				uor->written += rlen;
				if (uor->written >= (size_t) uor->ubuf->pos) {
					uwsgi_log("done written\n");
					// reset buffer
					uor->ubuf->pos = 0;
					// back to wait
					uor->status = 4;
					if (event_queue_add_fd_read(ut->queue, uor->fd))
						return -1;
					if (event_queue_add_fd_read(ut->queue, uor->fd2))
						return -1;
					if (event_queue_fd_write_to_read(ut->queue, uor->s))
						return -1;
				}
				return 0;
			}
			else if (rlen < 0) {
				uwsgi_offload_retry uwsgi_error("realtime_websocket_offload_do() -> write()");
			}
		}
		return -1;
	case 6:
		// publish message to redis
		// write received message to the socket
		if (fd == uor->fd2) {
			ssize_t rlen = write(uor->fd2, uor->ubuf->buf + uor->written, uor->ubuf->pos - uor->written);
			if (rlen > 0) {
				uor->written += rlen;
				if (uor->written >= (size_t) uor->ubuf->pos) {
					// reset buffer
					uor->ubuf->pos = 0;
					// back to wait
					uor->status = 4;
					if (event_queue_add_fd_read(ut->queue, uor->fd))
						return -1;
					if (event_queue_add_fd_read(ut->queue, uor->s))
						return -1;
					if (event_queue_fd_write_to_read(ut->queue, uor->fd2))
						return -1;
				}
				return 0;
			}
			else if (rlen < 0) {
				uwsgi_offload_retry uwsgi_error("realtime_websocket_offload_do() -> write()");
			}
		}
		return -1;
	default:
		return -1;
	}
	return -1;
}
