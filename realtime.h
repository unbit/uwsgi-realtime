#include <uwsgi.h>

#define uwsgi_offload_retry if (uwsgi_is_again()) return 0;

#define REALTIME_RAW 0
#define REALTIME_SSE 1
#define REALTIME_SOCKETIO 2
#define REALTIME_WEBSOCKET 3
#define REALTIME_ISTREAM 5
#define REALTIME_UPLOAD 6
#define REALTIME_INTERLEAVED 7
#define REALTIME_CHUNKED 8
#define REALTIME_RTMPT 9

ssize_t urt_redis_pubsub(char *, size_t, int64_t *, char **);
ssize_t urt_redis_parse(char *, size_t, char *, int64_t *, char **);

int realtime_redis_offload_engine_prepare(struct wsgi_request *, struct uwsgi_offload_request *);
int realtime_redis_offload_engine_do(struct uwsgi_thread *, struct uwsgi_offload_request *, int);

char *sse_build(char *, int64_t, uint64_t *);
int eio_build(struct uwsgi_buffer *);
int eio_build_http(struct uwsgi_buffer *);
int eio_body_publish(struct wsgi_request *);

int realtime_redis_publish(char *, size_t, char *, size_t);
int realtime_redis_build_publish(struct uwsgi_buffer *, char *, size_t, char *, size_t);

ssize_t realtime_websocket_parse(struct uwsgi_buffer *, uint8_t *, char **, uint64_t *);

int realtime_websocket_offload_do(struct uwsgi_thread *, struct uwsgi_offload_request *, int);

int socketio_router_func(struct wsgi_request *, struct uwsgi_route *);

int realtime_redis_offload(struct wsgi_request *, char *, uint16_t, uint64_t);
int realtime_istream_offload_do(struct uwsgi_thread *, struct uwsgi_offload_request *, int);
int realtime_sse_offload_do(struct uwsgi_thread *, struct uwsgi_offload_request *, int);

int realtime_write_buf(struct uwsgi_thread *ut, struct uwsgi_offload_request *);
int realtime_subscribe_ubuf(struct uwsgi_thread *ut, struct uwsgi_offload_request *, int);

int upload_router_func(struct wsgi_request *, struct uwsgi_route *);
int realtime_upload_offload_engine_prepare(struct wsgi_request *, struct uwsgi_offload_request *);

int realtime_upload_offload_engine_do(struct uwsgi_thread *, struct uwsgi_offload_request *, int);

int interleaved_router_func(struct wsgi_request *, struct uwsgi_route *);
int realtime_interleaved_offload_engine_prepare(struct wsgi_request *, struct uwsgi_offload_request *);

int realtime_interleaved_offload_engine_do(struct uwsgi_thread *, struct uwsgi_offload_request *, int);

int webm_router_func(struct wsgi_request *, struct uwsgi_route *);
