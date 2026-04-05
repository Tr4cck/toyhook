#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <elf.h>

#include "test_framework.h"

/* ── mock stubs for PLT hook dependencies ───────────────────── */

static unsigned long stub_module_base = 0x400000;
unsigned long find_module_base(const char *path, const char *mod) {
    (void)path; (void)mod;
    return stub_module_base;
}

static Elf64_Dyn stub_dyn;
static Elf64_Dyn *stub_dyn_section = &stub_dyn;
Elf64_Dyn *find_dynamic_section(unsigned long base) {
    (void)base;
    return stub_dyn_section;
}

static int stub_got_slot_ret = -1;
static void **stub_got_slot_ptr;
int find_got_slot(unsigned long base, Elf64_Dyn *dyn,
                  const char *sym_name, void ***out_slot) {
    (void)base; (void)dyn; (void)sym_name;
    if (stub_got_slot_ret == 0 && out_slot)
        *out_slot = stub_got_slot_ptr;
    return stub_got_slot_ret;
}

static int stub_make_rw_ret = 0;
static int stub_make_rw_orig_prot = PROT_READ | PROT_EXEC;
static unsigned long stub_make_rw_last_addr = 0;
static long stub_make_rw_last_size = 0;
int make_page_rw(unsigned long addr, long page_size) {
    stub_make_rw_last_addr = addr;
    stub_make_rw_last_size = page_size;
    if (stub_make_rw_ret != 0) return stub_make_rw_ret;
    return stub_make_rw_orig_prot;
}

long toy_pagesize(void) {
    return getpagesize();
}

static int stub_restore_called = 0;
static unsigned long stub_restore_last_addr = 0;
static int stub_restore_last_prot = 0;
int restore_page_perms(unsigned long addr, long page_size, int prot) {
    stub_restore_called = 1;
    stub_restore_last_addr = addr;
    stub_restore_last_prot = prot;
    (void)page_size;
    return 0;
}

static void reset_stubs(void) {
    stub_module_base = 0x400000;
    stub_dyn_section = &stub_dyn;
    stub_got_slot_ret = -1;
    stub_got_slot_ptr = NULL;
    stub_make_rw_ret = 0;
    stub_make_rw_orig_prot = PROT_READ | PROT_EXEC;
    stub_make_rw_last_addr = 0;
    stub_make_rw_last_size = 0;
    stub_restore_called = 0;
    stub_restore_last_addr = 0;
    stub_restore_last_prot = 0;
}

/* ── include code under test ────────────────────────────────── */
#include "../backend/plt_hook.c"

/* ── helpers ──────────────────────────────────────────────── */

/*
 * alloc_got_page — allocate a RW page with one "GOT slot" inside it.
 *
 * Returns a page where got_page[0] is a writable GOT slot pre-filled
 * with `original_value`.  The caller must munmap the page when done.
 */
static void **alloc_got_page(void *original_value) {
    void **page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return NULL;
    page[0] = original_value;
    return page;
}

static void free_got_page(void **page) {
    if (page) munmap(page, 4096);
}

/* ── hook_plt_symbol tests ─────────────────────────────────── */

TEST(hook_plt_module_not_found) {
    reset_stubs();
    stub_module_base = 0;
    void *orig = NULL;
    ASSERT_INT(hook_plt_symbol("no_such.so", "printf", (void*)0xAAAA, &orig), -1);
    return 0;
}

TEST(hook_plt_no_dynamic_section) {
    reset_stubs();
    stub_dyn_section = NULL;
    void *orig = NULL;
    ASSERT_INT(hook_plt_symbol("libc.so", "printf", (void*)0xAAAA, &orig), -1);
    return 0;
}

TEST(hook_plt_symbol_not_in_got) {
    reset_stubs();
    stub_got_slot_ret = -1;
    void *orig = NULL;
    ASSERT_INT(hook_plt_symbol("libc.so", "nonexistent_sym", (void*)0xAAAA, &orig), -1);
    return 0;
}

TEST(hook_plt_make_page_rw_fails) {
    reset_stubs();
    void **got = alloc_got_page((void*)0x1234);
    ASSERT_TRUE(got != NULL);

    stub_got_slot_ret = 0;
    stub_got_slot_ptr = got;
    stub_make_rw_ret = -1;

    void *orig = NULL;
    ASSERT_INT(hook_plt_symbol("libc.so", "printf", (void*)0xAAAA, &orig), -1);
    free_got_page(got);
    return 0;
}

TEST(hook_plt_patches_got_and_saves_original) {
    reset_stubs();
    void **got = alloc_got_page((void*)0xBEEFCAFE);
    ASSERT_TRUE(got != NULL);

    stub_got_slot_ret = 0;
    stub_got_slot_ptr = got;
    stub_make_rw_orig_prot = PROT_READ;

    void *orig = NULL;
    ASSERT_INT(hook_plt_symbol("libc.so", "printf", (void*)0xAAAA1111, &orig), 0);

    ASSERT_PTR(got[0], (void*)0xAAAA1111);
    ASSERT_PTR(orig, (void*)0xBEEFCAFE);
    free_got_page(got);
    return 0;
}

TEST(hook_plt_calls_make_page_rw) {
    reset_stubs();
    void **got = alloc_got_page((void*)0x1234);
    ASSERT_TRUE(got != NULL);

    stub_got_slot_ret = 0;
    stub_got_slot_ptr = got;

    void *orig = NULL;
    hook_plt_symbol("libc.so", "printf", (void*)0xAAAA, &orig);

    ASSERT_PTR((void*)stub_make_rw_last_addr, (void*)got);
    ASSERT_INT(stub_make_rw_last_size, getpagesize());
    free_got_page(got);
    return 0;
}

TEST(hook_plt_restores_page_perms) {
    reset_stubs();
    void **got = alloc_got_page((void*)0x1234);
    ASSERT_TRUE(got != NULL);

    stub_got_slot_ret = 0;
    stub_got_slot_ptr = got;
    stub_make_rw_orig_prot = PROT_READ | PROT_EXEC;

    void *orig = NULL;
    hook_plt_symbol("libc.so", "printf", (void*)0xAAAA, &orig);

    ASSERT_TRUE(stub_restore_called);
    ASSERT_PTR((void*)stub_restore_last_addr, (void*)got);
    ASSERT_INT(stub_restore_last_prot, PROT_READ | PROT_EXEC);
    free_got_page(got);
    return 0;
}

/* ── unhook_plt_symbol tests ───────────────────────────────── */

TEST(unhook_plt_module_not_found) {
    reset_stubs();
    stub_module_base = 0;
    ASSERT_INT(unhook_plt_symbol("no_such.so", "printf", (void*)0x1234), -1);
    return 0;
}

TEST(unhook_plt_no_dynamic_section) {
    reset_stubs();
    stub_dyn_section = NULL;
    ASSERT_INT(unhook_plt_symbol("libc.so", "printf", (void*)0x1234), -1);
    return 0;
}

TEST(unhook_plt_symbol_not_in_got) {
    reset_stubs();
    stub_got_slot_ret = -1;
    ASSERT_INT(unhook_plt_symbol("libc.so", "nonexistent", (void*)0x1234), -1);
    return 0;
}

TEST(unhook_plt_restores_got_slot) {
    reset_stubs();
    void **got = alloc_got_page((void*)0xDEAD);
    ASSERT_TRUE(got != NULL);
    got[0] = (void*)0xAAAA;

    stub_got_slot_ret = 0;
    stub_got_slot_ptr = got;
    stub_make_rw_orig_prot = PROT_READ;

    ASSERT_INT(unhook_plt_symbol("libc.so", "printf", (void*)0xDEAD), 0);

    ASSERT_PTR(got[0], (void*)0xDEAD);
    ASSERT_TRUE(stub_restore_called);
    ASSERT_INT(stub_restore_last_prot, PROT_READ);
    free_got_page(got);
    return 0;
}

TEST(unhook_plt_make_page_rw_fails) {
    reset_stubs();
    void **got = alloc_got_page((void*)0xAAAA);
    ASSERT_TRUE(got != NULL);

    stub_got_slot_ret = 0;
    stub_got_slot_ptr = got;
    stub_make_rw_ret = -1;

    ASSERT_INT(unhook_plt_symbol("libc.so", "printf", (void*)0xDEAD), -1);
    ASSERT_PTR(got[0], (void*)0xAAAA);
    free_got_page(got);
    return 0;
}

/* ── hook then unhook round-trip ─────────────────────────────── */

TEST(hook_then_unhook_roundtrip) {
    reset_stubs();
    void **got = alloc_got_page((void*)0xBEEFCAFE);
    ASSERT_TRUE(got != NULL);

    stub_got_slot_ret = 0;
    stub_got_slot_ptr = got;
    stub_make_rw_orig_prot = PROT_READ;

    void *orig = NULL;
    ASSERT_INT(hook_plt_symbol("libc.so", "printf", (void*)0xDEADBEEF, &orig), 0);
    ASSERT_PTR(got[0], (void*)0xDEADBEEF);
    ASSERT_PTR(orig, (void*)0xBEEFCAFE);
    stub_restore_called = 0;
    ASSERT_INT(unhook_plt_symbol("libc.so", "printf", orig), 0);
    ASSERT_PTR(got[0], (void*)0xBEEFCAFE);
    ASSERT_TRUE(stub_restore_called);
    free_got_page(got);
    return 0;
}

TEST(hook_plt_double_patches_twice) {
    reset_stubs();
    void **got = alloc_got_page((void*)0x11111111);
    ASSERT_TRUE(got != NULL);
    stub_got_slot_ret = 0;
    stub_got_slot_ptr = got;
    stub_make_rw_orig_prot = PROT_READ;

    void *orig1 = NULL;
    hook_plt_symbol("libc.so", "printf", (void*)0x22222222, &orig1);
    ASSERT_PTR(orig1, (void*)0x11111111);
    stub_restore_called = 0;
    stub_make_rw_orig_prot = PROT_READ;
    void *orig2 = NULL;
    hook_plt_symbol("libc.so", "printf", (void*)0x33333333, &orig2);
    ASSERT_PTR(orig2, (void*)0x22222222);
    ASSERT_PTR(got[0], (void*)0x33333333);
    free_got_page(got);
    return 0;
}

/* ── main ─────────────────────────────────────────────────── */

int main(void) {
    printf("PLT hook tests:\n");

    RUN_TEST(hook_plt_module_not_found);
    RUN_TEST(hook_plt_no_dynamic_section);
    RUN_TEST(hook_plt_symbol_not_in_got);
    RUN_TEST(hook_plt_make_page_rw_fails);
    RUN_TEST(hook_plt_patches_got_and_saves_original);
    RUN_TEST(hook_plt_calls_make_page_rw);
    RUN_TEST(hook_plt_restores_page_perms);

    RUN_TEST(unhook_plt_module_not_found);
    RUN_TEST(unhook_plt_no_dynamic_section);
    RUN_TEST(unhook_plt_symbol_not_in_got);
    RUN_TEST(unhook_plt_restores_got_slot);
    RUN_TEST(unhook_plt_make_page_rw_fails);

    RUN_TEST(hook_then_unhook_roundtrip);
    RUN_TEST(hook_plt_double_patches_twice);

    printf("\n%d/%d passed, %d failed\n", __tf_pass, __tf_total, __tf_fail);
    return __tf_fail > 0 ? 1 : 0;
}
