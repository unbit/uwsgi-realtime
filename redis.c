#include "realtime.h"

extern struct uwsgi_server uwsgi;

ssize_t urt_redis_num(char *buf, size_t len, int64_t *n) {
	char *ptr = buf;
	int is_negative = 0;
	int64_t num = 0;

	if (*ptr == '-') {
		is_negative = 1;
		ptr++;
		len--;
	}

	for(;;) {
		// more ?
		if (len == 0) return 0;
		if (*ptr == '\r') {
			break;
		}
		if (!isdigit((int) (*ptr))) {
			return -1;
		}
		num = (num*10)+((*ptr) - '0');
		ptr++;
		len--;
	}

	ptr++;
	len--;
	if (len == 0) return 0;
	if (*ptr != '\n') return -1;

	ptr++;

	if (is_negative) {
		num = -num;
	}

	*n = num;
	return ptr - buf;	
}


ssize_t urt_redis_string(char *buf, size_t len) {
        char *ptr = buf;

        for(;;) {
                // more ?
                if (len == 0) return 0;
                if (*ptr == '\r') {
                        break;
                }
                ptr++;
                len--;
        }

        ptr++;
        len--;  
        if (len == 0) return 0;
        if (*ptr != '\n') return -1;

	ptr++;

        return ptr - buf;
}

ssize_t urt_redis_bulk(char *buf, size_t len, char **str, int64_t *str_len) {
	ssize_t ret = urt_redis_num(buf, len, str_len);
	if (ret <= 0) return ret;
	buf += ret; len -= ret;
	char *ptr = buf;
	*str = buf;
	int64_t n = *str_len;
	while(n > 0) {
		// more
		if (len == 0) return 0;
                ptr++;
                len--;	
		n--;
	}

	if (len == 0) return 0;
	if (*ptr != '\r') return -1;
        ptr++;
        len--;	
	if (len == 0) return 0;
	if (*ptr != '\n') return -1;

	ptr++;

	return (ptr - buf) + ret;
}

ssize_t urt_redis_parse(char *buf, size_t len, char *type, int64_t *n, char **str) {
	if (len == 0) return 0;
	*type = *buf;
	buf++;
	len--;
	if (len == 0) return 0;

	ssize_t ret, array_ret;
	int64_t i;

	switch(*type) {
		// simple string
                case '+':
		// error
                case '-':
			ret = urt_redis_string(buf, len);
			break;
                // int
                case ':':
			ret = urt_redis_num(buf, len, n);
			break;
                // bulk strings
                case '$':
			ret = urt_redis_bulk(buf, len, str, n);
			break;
                // array
                case '*':
			array_ret = urt_redis_num(buf, len, n);
			if (array_ret <= 0) return array_ret;
			buf += array_ret;
			len -= array_ret;
			ret = array_ret;
                        for(i=0;i<(*n);i++) {
				char array_type;
				int64_t array_n;
				char *array_str;
				array_ret = urt_redis_parse(buf, len, &array_type, &array_n, &array_str);
				if (array_ret <= 0) return array_ret;
				buf += array_ret;
                        	len -= array_ret;
                        	ret += array_ret;
                        }
			break;
                default:
                        return -1;
	}

	if (ret > 0) ret++;
	return ret;
}

ssize_t urt_redis_pubsub(char *buf, size_t len, int64_t *n, char **str) {
	*n = 0;
	if (len == 0) return 0;
	char *ptr = buf;
	if (*ptr != '*') return -1;
	ptr++;
        len--;
        if (len == 0) return 0;

	int64_t array_n = 0;
	ssize_t array_ret = urt_redis_num(ptr, len, &array_n);
	if (array_ret <= 0) return array_ret;

	ptr += array_ret;
        len -= array_ret;

	if (array_n != 3) return -1;

	if (len == 0) return 0;
        if (*ptr != '$') return -1;
        ptr++; len--;

	char *array_str;
	array_ret = urt_redis_bulk(ptr, len, &array_str, &array_n);
	if (array_ret <= 0) return array_ret;

        ptr += array_ret;
        len -= array_ret;

	if (array_n <= 0) return -1;

	if (!uwsgi_strncmp(array_str, array_n, "message", 7)) {
		if (len == 0) return 0;
		if (*ptr != '$') return -1;
		ptr++; len--;
		array_ret = urt_redis_bulk(ptr, len, &array_str, &array_n);
        	if (array_ret <= 0) return array_ret;

		ptr += array_ret;
        	len -= array_ret;

		if (len == 0) return 0;
                if (*ptr != '$') return -1;
                ptr++; len--;
		// directly set output
                array_ret = urt_redis_bulk(ptr, len, str, n);
                if (array_ret <= 0) return array_ret;

		ptr += array_ret;
	}
	else {
		// ignore
		char type;
		array_ret = urt_redis_parse(ptr, len, &type, &array_n, &array_str);
		if (array_ret <= 0) return array_ret;
        	ptr += array_ret;
        	len -= array_ret;

		if (len == 0) return 0;

		array_ret = urt_redis_parse(ptr, len, &type, &array_n, &array_str);
                if (array_ret <= 0) return array_ret;
                ptr += array_ret;
	}

	return ptr - buf;
}

int realtime_redis_build_publish(struct uwsgi_buffer *ub, char *buf, size_t len, struct realtime_config *rc) {
	if (!rc->publish_len) return -1;
	if (uwsgi_buffer_append(ub, "*3\r\n$7\r\nPUBLISH\r\n$", 18)) return -1;
	if (uwsgi_buffer_num64(ub, rc->publish_len)) return -1;
        if (uwsgi_buffer_append(ub, "\r\n", 2)) return -1;
        if (uwsgi_buffer_append(ub, rc->publish, rc->publish_len)) return -1;
        if (uwsgi_buffer_append(ub, "\r\n$", 3)) return -1;
        if (uwsgi_buffer_num64(ub, rc->prefix_len + len + rc->suffix_len)) return -1;
        if (uwsgi_buffer_append(ub, "\r\n", 2)) return -1;
	if (rc->prefix_len > 0) {
        	if (uwsgi_buffer_append(ub, rc->prefix, rc->prefix_len)) return -1;
	}
        if (uwsgi_buffer_append(ub, buf, len)) return -1;
	if (rc->suffix_len > 0) {
        	if (uwsgi_buffer_append(ub, rc->suffix, rc->suffix_len)) return -1;
	}
        if (uwsgi_buffer_append(ub, "\r\n", 2)) return -1;
	return 0;
}

int realtime_redis_rtp_publish(struct uwsgi_buffer *ub, uint8_t channel, uint64_t ts, char *buf, size_t len, struct realtime_config *rc) {
        if (!rc->publish_len) return -1;
        if (uwsgi_buffer_append(ub, "*3\r\n$7\r\nPUBLISH\r\n$", 18)) return -1;
        if (uwsgi_buffer_num64(ub, rc->publish_len)) return -1;
        if (uwsgi_buffer_append(ub, "\r\n", 2)) return -1;
        if (uwsgi_buffer_append(ub, rc->publish, rc->publish_len)) return -1;
        if (uwsgi_buffer_append(ub, "\r\n$", 3)) return -1;
        if (uwsgi_buffer_num64(ub, 9 + len)) return -1;
        if (uwsgi_buffer_append(ub, "\r\n", 2)) return -1;
	if (uwsgi_buffer_u8(ub, channel)) return -1;
	if (uwsgi_buffer_u64be(ub, ts)) return -1;
        if (uwsgi_buffer_append(ub, buf, len)) return -1;
        if (uwsgi_buffer_append(ub, "\r\n", 2)) return -1;
        return 0;
}

int realtime_redis_publish(struct realtime_config *rc, char *buf, size_t len) {
	struct uwsgi_buffer *ub = uwsgi_buffer_new(uwsgi.page_size);
	if (realtime_redis_build_publish(ub, buf, len, rc)) goto error;	

	int fd = uwsgi_connect(rc->server, 0, 1);
	if (fd < 0) goto error;

	if (uwsgi.wait_write_hook(fd, uwsgi.socket_timeout) <= 0) {
		close(fd);
		goto error;
	}

	if (uwsgi_write_true_nb(fd, ub->buf, ub->pos, uwsgi.socket_timeout)) {
		close(fd);
		goto error;
	}
	close(fd);
	uwsgi_buffer_destroy(ub);
	return 0;
error:
	uwsgi_buffer_destroy(ub);
	return -1;
}
