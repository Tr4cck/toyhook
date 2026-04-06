#include "mem.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include "log.h"

static long cached_pagesize;

long toy_pagesize(void) {
    if (!cached_pagesize)
        cached_pagesize = sysconf(_SC_PAGESIZE);
    return cached_pagesize;
}

unsigned long page_of(unsigned long addr) {
    return addr & ~(unsigned long)(toy_pagesize() - 1);
}

char *get_page_perms(unsigned long addr) {
    unsigned long page = page_of(addr);
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return NULL;

    char line[512];
    char *result = NULL;
    while (fgets(line, sizeof(line), f)) {
        unsigned long start, end;
        char perms[8] = {};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) >= 3) {
            if (page >= start && page < end) {
                result = strdup(perms);
                break;
            }
        }
    }
    fclose(f);
    return result;
}

int perms_to_prot(const char *perms) {
    int prot = 0;
    if (perms[0] == 'r') prot |= PROT_READ;
    if (perms[1] == 'w') prot |= PROT_WRITE;
    if (perms[2] == 'x') prot |= PROT_EXEC;
    return prot;
}

int make_page_rw(unsigned long addr, long page_size) {
    unsigned long page = page_of(addr);

    char *perms = get_page_perms(addr);
    if (!perms) {
        LOGE("could not read perms for %p", (void *)page);
        return -1;
    }

    int orig_prot = perms_to_prot(perms);
    free(perms);

    if (mprotect((void *)page, page_size, orig_prot | PROT_WRITE) != 0) {
        LOGE("mprotect RW for %p: %s", (void *)page, strerror(errno));
        return -1;
    }
    return orig_prot;
}

int restore_page_perms(unsigned long addr, long page_size, int orig_prot) {
    unsigned long page = addr & ~(unsigned long)(page_size - 1);
    if (mprotect((void *)page, page_size, orig_prot) != 0) {
        LOGE("mprotect restore for %p: %s", (void *)page, strerror(errno));
        return -1;
    }
    return 0;
}

void *alloc_rwx(size_t size) {
    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        LOGE("mmap rwx: %s", strerror(errno));
        return NULL;
    }
    LOGD("rwx page allocated at %p (%zu bytes)", mem, size);
    return mem;
}

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

struct mapping { size_t start, end; };

bool is_addr_in_gap(unsigned long addr, struct mapping *mappings, size_t count) {
    for (size_t i = 1; i < count; i++) {
        if (addr > mappings[i-1].end && addr < mappings[i].start)
            return true;
    }
    return false;
}

void *alloc_rwx_near(size_t size, size_t near_addr) {
    /* Goal: allocate an RWX page within ±128MB of near_addr so that
     * PC-relative instructions in stolen code can reach their targets.
     *
     * Algorithm outline:
     * 1. Parse /proc/self/maps to collect all mapped regions as (start, end) pairs
     * 2. Start from page_of(near_addr), sweep outward: 0, +ps, -ps, +2*ps, -2*ps, ...
     * 3. For each candidate page, check if it falls in a gap between mappings
     * 4. Use mmap(addr, ..., MAP_FIXED_NOREPLACE) to safely claim the gap
     * 5. Return the mapped page, or NULL if no gap found within ±128MB
     *
     * Key constraints:
     *   TBZ/TBNZ: ±32KB, CBZ/CBNZ/B.cond: ±1MB, B/BL: ±128MB
     *   Search radius 128MB covers all branch types
     *   MAP_FIXED_NOREPLACE (Linux 4.17+) won't destroy existing mappings
     *   Trampoline needs only ~64 bytes but you get a full 4KB page
     */
    
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        LOGE("failed to open /proc/self/maps: %s", strerror(errno));
        return NULL;
    }

    char line[512];
    size_t cap = 256, count = 0;
    struct mapping *mappings = malloc(cap * sizeof(struct mapping));
    while (fgets(line, sizeof(line), f)) {
        size_t start, end;
        if (sscanf(line, "%zx-%zx", &start, &end) == 2) {
            if (count >= cap) {
                cap *= 2;
                mappings = realloc(mappings, cap * sizeof(*mappings));
            }
            mappings[count++] = (struct mapping){start, end};
        }
    }
    fclose(f);

    size_t near_page = page_of(near_addr);
    size_t ps = toy_pagesize();
    size_t r  = 128 * 1024 * 1024; // 128MB search radius

    for (size_t off = 0; off <= r; off += ps) {
        for (int d = 0; d < 2; d++) { // d = 0: up, d = 1: down
            if (off == 0 && d == 1)
                continue; // Don't check the same page twice

            size_t candidate = (d == 0) ? near_page + off : near_page - off;
            if (candidate < (uintptr_t) near_page - r || candidate > (uintptr_t) near_page + r)
                continue; // Out of bounds

            if (is_addr_in_gap(candidate, mappings, count)) {
                LOGD("candidate %p is in a gap, trying to mmap", (void *)candidate);
                void *mem = mmap((void *)candidate, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (mem != MAP_FAILED) {
                    LOGD("successfully allocated rwx page at %p near %p", mem, (void *)near_addr);
                    free(mappings);
                    return mem;
                } else {
                    LOGE("mmap rwx at candidate %p failed: %s", (void *)candidate, strerror(errno));
                }
            }
        }
    }
    free(mappings);
    return NULL;
}