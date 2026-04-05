/*
 * toyhook injector - remote dlopen via ptrace (ARM64)
 *
 * Principle:
 *   1. ptrace attach to target, freeze it
 *   2. Resolve dlopen's address inside the target process
 *   3. Write SO path string into target's stack area
 *   4. Hijack target's registers: PC -> dlopen, x0 -> path, x30 -> trap
 *   5. Resume target; dlopen loads our SO, then returns to trap address
 *   6. Catch the fault signal, restore original state, detach
 *
 * No shellcode needed. We manipulate registers directly.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <elf.h>
#include "utils/elf.h"
#include "utils/log.h"

#define PATH_BUF_SIZE 256

/*
 * ARM64 user pt_regs layout (NT_PRSTATUS):
 *   regs[0..30] = x0..x30  (x30 = LR / link register)
 *   sp           = stack pointer
 *   pc           = program counter
 *   pstate       = processor state
 */
typedef struct {
    unsigned long regs[31];
    unsigned long sp;
    unsigned long pc;
    unsigned long pstate;
} regs_t;

static int get_regs(pid_t pid, regs_t *regs) {
    struct iovec iov = { .iov_base = regs, .iov_len = sizeof(*regs) };
    return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

static int set_regs(pid_t pid, const regs_t *regs) {
    struct iovec iov = { .iov_base = (void *)regs, .iov_len = sizeof(*regs) };
    return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

static int read_mem(pid_t pid, unsigned long addr, void *buf, size_t len) {
    for (size_t i = 0; i < len; i += sizeof(long)) {
        errno = 0;
        long val = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);
        if (val == -1 && errno)
            return -1;
        *(long *)((char *)buf + i) = val;
    }
    return 0;
}

/*
 * write_mem - write arbitrary bytes into target process memory via ptrace.
 *
 * PTRACE_POKEDATA writes one machine word (8 bytes on ARM64) at a time.
 * For tail bytes that don't fill a full word:
 *   1. PEEKDATA the existing word (read-modify-write)
 *   2. overlay our bytes onto it
 *   3. POKEDATA the merged word back
 *
 *   word boundary:  |<-------- existing word -------->|
 *   our data:                 |<-- tail -->|
 *   result:         |<-- kept -->|<-- new -->|  (rest zeroed by memset)
 */
static int write_mem(pid_t pid, unsigned long addr, const void *buf, size_t len) {
    for (size_t i = 0; i < len; i += sizeof(long)) {
        long val = 0;
        size_t chunk = sizeof(long);
        if (len - i < chunk) {
            errno = 0;
            val = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), NULL);
            if (val == -1 && errno)
                return -1;
            chunk = len - i;
        }
        memcpy(&val, (const char *)buf + i, chunk);
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(addr + i), (void *)val) == -1)
            return -1;
    }
    return 0;
}

/*
 * resolve_remote_dlopen - find dlopen's address in the TARGET process.
 *
 * Same .so file, same internal layout. ASLR only randomizes the base address,
 * not the offset of symbols within the file. So:
 *
 *   OUR process:                          TARGET process:
 *   +-------------+ 0x7ac0000000          +-------------+ 0x7b10000000  (base, different)
 *   | linker64    |                       | linker64    |
 *   |   ...       |                       |   ...       |
 *   | dlopen:     |                       | dlopen:     |
 *   | +0x1f014    | <-- offset            | +0x1f014    | <-- SAME offset
 *   |   ...       |                       |   ...       |
 *   +-------------+                       +-------------+
 *
 *   my_dlopen = my_base + offset          target_dlopen = target_base + offset
 *   => offset = my_dlopen - my_base       => target_dlopen = target_base + offset
 */
static unsigned long resolve_remote_dlopen(pid_t pid) {
    void *sym = dlsym(RTLD_DEFAULT, "dlopen");
    if (!sym) {
        LOGE("dlsym(dlopen): %s", dlerror());
        return 0;
    }

    Dl_info info;
    if (!dladdr(sym, &info) || !info.dli_fname) {
        LOGE("dladdr failed");
        return 0;
    }

    const char *libname = strrchr(info.dli_fname, '/');
    libname = libname ? libname + 1 : info.dli_fname;

    unsigned long offset = (unsigned long)sym - (unsigned long)info.dli_fbase;

    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    unsigned long target_base = find_module_base(maps_path, libname);
    if (!target_base) {
        LOGE("cannot find %s in target", libname);
        return 0;
    }

    return target_base + offset;
}

/*
 * inject - the main injection routine.
 *
 * Stack memory layout we set up in the target:
 *
 *   high address
 *   +---------------------------+ <- orig_regs.sp (original stack pointer)
 *   | target's active frames    |    (don't touch)
 *   +---------------------------+
 *   | padding (0-7 bytes)       |    alignment to 8B
 *   +---------------------------+ <- path_addr = (sp - 256) & ~7
 *   | path string:              |
 *   | "/data/local/tmp/..."     |    write_mem puts it here
 *   |       256 bytes           |
 *   +---------------------------+
 *   | padding (0-15 bytes)      |    alignment to 16B
 *   +---------------------------+ <- new_sp = (path_addr - 4096) & ~15
 *   | dlopen stack workspace    |    dlopen grows stack downward from here
 *   |       ~4KB                |
 *   +---------------------------+
 *   low address
 *
 * Register setup:
 *   x0  = path_addr                  first argument: path to .so
 *   x1  = RTLD_NOW | RTLD_GLOBAL     second argument: flags
 *   x30 = 0xDEADDEAD                 link register: trap on return
 *   pc  = dlopen_addr                jump directly into dlopen
 *   sp  = new_sp                     16-byte aligned, below path string
 *
 * When dlopen finishes and executes `ret`, it jumps to x30 = 0xDEADDEAD,
 * which is unmapped -> SIGSEGV or SIGBUS -> we catch it as "done".
 */
static int inject(pid_t pid, const char *so_path) {
    regs_t orig_regs;
    unsigned long dlopen_addr, path_addr;
    char pathbuf[PATH_BUF_SIZE];
    int status, ret = -1;

    LOGI("attaching to %d", pid);
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1) {
        LOGE("PTRACE_ATTACH: %s", strerror(errno));
        return -1;
    }
    waitpid(pid, NULL, 0);
    LOGI("attached");

    if (get_regs(pid, &orig_regs) == -1) {
        LOGE("get_regs: %s", strerror(errno));
        goto detach;
    }

    LOGI("resolving dlopen");
    dlopen_addr = resolve_remote_dlopen(pid);
    if (!dlopen_addr) {
        LOGE("failed to resolve dlopen");
        goto detach;
    }
    LOGI("dlopen @ 0x%lx", dlopen_addr);

    /* Write the SO path into target's stack, just below the original SP.
     * & ~7UL clears bits 0-2 -> rounds down to 8-byte alignment.
     * Required because PTRACE_POKEDATA operates on 8-byte word boundaries. */
    path_addr = (orig_regs.sp - PATH_BUF_SIZE) & ~7UL;
    memset(pathbuf, 0, sizeof(pathbuf));
    strncpy(pathbuf, so_path, sizeof(pathbuf) - 1);
    if (write_mem(pid, path_addr, pathbuf, sizeof(pathbuf)) == -1) {
        LOGE("failed to write path string");
        goto detach;
    }

    regs_t call_regs = orig_regs;
    call_regs.regs[0]  = path_addr;                       // x0  = path
    call_regs.regs[1]  = RTLD_NOW | RTLD_GLOBAL;          // x1  = flags
    call_regs.regs[30] = 0xDEADDEADUL;                    // x30 = trap on return
    call_regs.pc       = dlopen_addr;                     // pc  = dlopen
    /* & ~15UL clears bits 0-3 -> rounds down to 16-byte alignment.
     * ARM64 AAPCS64 mandates SP must be 16-byte aligned. */
    call_regs.sp       = (path_addr - 4096) & ~15UL;     // sp below path string
    if (set_regs(pid, &call_regs) == -1) {
        LOGE("set_regs: %s", strerror(errno));
        goto detach;
    }

    LOGI("calling remote dlopen(\"%s\")", so_path);
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) == -1) {
        LOGE("PTRACE_CONT: %s", strerror(errno));
        goto restore;
    }

    /* dlopen returns -> jumps to 0xDEADDEAD -> fault (SIGSEGV or SIGBUS).
     * ARM64 may deliver either signal depending on the CPU/memory controller. */
    waitpid(pid, &status, 0);
    int sig = WIFSTOPPED(status) ? WSTOPSIG(status) : 0;
    if (sig == SIGSEGV || sig == SIGBUS) {
        LOGI("dlopen returned (sig=%d)", sig);
        ret = 0;
    } else {
        LOGE("unexpected stop: status=0x%x sig=%d", status, sig);
    }

restore:
    set_regs(pid, &orig_regs);
detach:
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    LOGI("detached");
    return ret;
}

int main(int argc, char *argv[]) {
    if (getenv("TOYHOOK_WAIT_DEBUGGER")) {
        LOGI("waiting for debugger... pid=%d", getpid());
        sleep(30);
    }

    if (argc != 4 || strcmp(argv[1], "inject") != 0) {
        LOGE("usage: %s inject <pid> <path/to/lib.so>", argv[0]);
        return 1;
    }

    pid_t pid = atoi(argv[2]);
    if (pid <= 0) {
        LOGE("invalid pid: %s", argv[2]);
        return 1;
    }

    char *so_path = realpath(argv[3], NULL);
    if (!so_path) {
        LOGE("cannot resolve %s: %s", argv[3], strerror(errno));
        return 1;
    }

    int ret = inject(pid, so_path);
    free(so_path);
    return ret;
}
