#include "realtime.h"

char *sse_build(char *message, int64_t message_len, uint64_t *final_len) {
        int64_t i;
        struct uwsgi_buffer *ub = uwsgi_buffer_new(message_len);
        char *ptr = message;
        size_t len = 0;
        for(i=0;i<message_len;i++) {
                len++;
                if (message[i] == '\n') {
                        if (uwsgi_buffer_append(ub, "data: ", 6)) goto error;
                        if (uwsgi_buffer_append(ub, ptr, len)) goto error;
                        ptr = message+i+1;
                        len = 0;
                }
        }

        if (uwsgi_buffer_append(ub, "data: ", 6)) goto error;
        if (len > 0) {
                if (uwsgi_buffer_append(ub, ptr, len)) goto error;
        }
        if (uwsgi_buffer_append(ub, "\n\n", 2)) goto error;
        *final_len = ub->pos;
        char *buf = ub->buf;
        ub->buf = NULL;
        uwsgi_buffer_destroy(ub);
        return buf;
error:
        uwsgi_buffer_destroy(ub);
        return NULL;
}

