#include <uwsgi.h>

#define uwsgi_offload_retry if (uwsgi_is_again()) return 0;

#define REALTIME_RAW 0
#define REALTIME_SSE 1
#define REALTIME_SOCKETIO 2
#define REALTIME_WEBSOCKET 3

ssize_t urt_redis_pubsub(char *, size_t, int64_t *, char **);
ssize_t urt_redis_parse(char *, size_t, char *, int64_t *, char **);

int realtime_redis_offload_engine_prepare(struct wsgi_request *, struct uwsgi_offload_request *);
int realtime_redis_offload_engine_do(struct uwsgi_thread *, struct uwsgi_offload_request *, int);

char *sse_build(char *, int64_t, uint64_t *);
int eio_build(struct uwsgi_buffer *);
int eio_build_http(struct uwsgi_buffer *);
int eio_body_publish(struct wsgi_request *);

int realtime_redis_publish(char *, size_t, char *, size_t);

ssize_t realtime_websocket_parse(struct uwsgi_buffer *, uint8_t *, char **, uint64_t *);
