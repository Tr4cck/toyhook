#ifndef TOYHOOK_UTILS_MEM_H
#define TOYHOOK_UTILS_MEM_H

#include <stddef.h>

unsigned long page_of(unsigned long addr);
long toy_pagesize(void);
char *get_page_perms(unsigned long addr);
int perms_to_prot(const char *perms);
int make_page_rw(unsigned long addr, long page_size);
int restore_page_perms(unsigned long addr, long page_size, int orig_prot);
void *alloc_rwx(size_t size);
void *alloc_rwx_near(size_t size, size_t near_addr);

#endif
