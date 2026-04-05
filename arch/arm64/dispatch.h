#ifndef TOYHOOK_ARCH_ARM64_DISPATCH_H
#define TOYHOOK_ARCH_ARM64_DISPATCH_H

typedef struct toy_hook toy_hook_t;

void *alloc_dispatch_stub(toy_hook_t *h);
void free_dispatch_stub(void *stub, toy_hook_t *h);

unsigned long toy_dispatch(toy_hook_t *h, unsigned long *args, unsigned argc);

#endif
