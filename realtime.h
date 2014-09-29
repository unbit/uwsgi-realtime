#include <uwsgi.h>

ssize_t urt_redis_pubsub(char *, size_t, int64_t *, char **);
ssize_t urt_redis_parse(char *, size_t, char *, int64_t *, char **);

int realtime_redis_offload_engine_prepare(struct wsgi_request *, struct uwsgi_offload_request *);
int realtime_redis_offload_engine_do(struct uwsgi_thread *, struct uwsgi_offload_request *, int);

char *sse_build(char *, int64_t, uint64_t *);
