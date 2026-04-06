#ifndef TOYHOOK_ARCH_ARM64_DISPATCH_H
#define TOYHOOK_ARCH_ARM64_DISPATCH_H

#include <stdint.h>

typedef struct toy_hook toy_hook_t;

void *alloc_dispatch_stub(toy_hook_t *h);
void free_dispatch_stub(void *stub, toy_hook_t *h);

uint64_t toy_dispatch(toy_hook_t *h, uint64_t *args, unsigned argc);

#endif
