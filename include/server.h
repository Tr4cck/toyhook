#ifndef TOYHOOK_SERVER_H
#define TOYHOOK_SERVER_H

#include "toyhook.h"
#include "trace.h"

#define TOYHOOK_SOCK_NAME "toyhook"

typedef struct {
    toy_session_t *session;
    toy_tracer_t  *tracer;
} toyhook_server_ctx_t;

void *toyhook_server_run(void *arg);

#endif
