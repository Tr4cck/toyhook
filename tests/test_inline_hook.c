#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "test_framework.h"
#include "utils/mem.h"

/* ── include code under test ───────────────────────────────── */
#include "../backend/inline_hook.c"

/* ── instruction encode / decode helpers ───────────────────── */

static uint32_t make_adrp(int rd, int64_t page_off)
{
    int64_t imm = page_off >> 12;
    return (1u << 31) | ((imm & 0x3) << 29) | (0b10000u << 24)
           | (((imm >> 2) & 0x7FFFF) << 5) | (rd & 0x1F);
}

static uint64_t decode_adrp(uint32_t insn, uint64_t pc)
{
    uint32_t immlo = (insn >> 29) & 0x3;
    uint32_t immhi = (insn >> 5) & 0x7FFFF;
    int64_t imm = ((int64_t)(int32_t)((immhi << 2 | immlo) << 11)) >> 11;
    return (pc + (imm << 12)) & ~0xFFFUL;
}

static uint32_t make_adr(int rd, int64_t offset)
{
    return ((offset & 0x3) << 29) | (0b10000u << 24)
           | (((offset >> 2) & 0x7FFFF) << 5) | (rd & 0x1F);
}

static uint64_t decode_adr(uint32_t insn, uint64_t pc)
{
    uint32_t immlo = (insn >> 29) & 0x3;
    uint32_t immhi = (insn >> 5) & 0x7FFFF;
    int64_t imm = ((int64_t)(int32_t)((immhi << 2 | immlo) << 11)) >> 11;
    return pc + imm;
}

static uint32_t make_b(int64_t offset)
{
    return 0x14000000u | ((offset >> 2) & 0x3FFFFFF);
}

static uint64_t decode_b(uint32_t insn, uint64_t pc)
{
    int64_t imm = ((int64_t)(int32_t)((insn & 0x3FFFFFF) << 6)) >> 4;
    return pc + imm;
}

static uint32_t make_bl(int64_t offset)
{
    return 0x94000000u | ((offset >> 2) & 0x3FFFFFF);
}

static uint32_t make_bcond(int cond, int64_t offset)
{
    return 0x54000000u | (((offset >> 2) & 0x7FFFF) << 5) | (cond & 0xF);
}

static uint64_t decode_bcond(uint32_t insn, uint64_t pc)
{
    int64_t imm = ((int64_t)(int32_t)(((insn >> 5) & 0x7FFFF) << 13)) >> 11;
    return pc + imm;
}

static uint32_t make_ldr_lit(int rt, int64_t offset)
{
    return 0x58000000u | (((offset >> 2) & 0x7FFFF) << 5) | (rt & 0x1F);
}

static uint64_t decode_ldr_lit(uint32_t insn, uint64_t pc)
{
    int64_t imm = ((int64_t)(int32_t)(((insn >> 5) & 0x7FFFF) << 13)) >> 11;
    return pc + imm;
}

static uint32_t make_cbz(int rt, int64_t offset, int sf)
{
    uint32_t base = sf ? 0xB4000000u : 0x34000000u;
    return base | (((offset >> 2) & 0x7FFFF) << 5) | (rt & 0x1F);
}

static uint64_t decode_cbz(uint32_t insn, uint64_t pc)
{
    int64_t imm = ((int64_t)(int32_t)(((insn >> 5) & 0x7FFFF) << 13)) >> 11;
    return pc + imm;
}

static uint32_t make_cbnz(int rt, int64_t offset, int sf)
{
    uint32_t base = sf ? 0xB5000000u : 0x35000000u;
    return base | (((offset >> 2) & 0x7FFFF) << 5) | (rt & 0x1F);
}

static uint32_t make_tbz(int rt, int bit, int64_t offset)
{
    return (((uint32_t)(bit >> 5) & 1) << 31) | 0x36000000u
           | ((bit & 0x1F) << 19)
           | (((offset >> 2) & 0x3FFF) << 5) | (rt & 0x1F);
}

static uint64_t decode_tbz(uint32_t insn, uint64_t pc)
{
    int64_t imm = ((int64_t)(int32_t)(((insn >> 5) & 0x3FFF) << 18)) >> 16;
    return pc + imm;
}

static uint32_t make_tbnz(int rt, int bit, int64_t offset)
{
    return (((uint32_t)(bit >> 5) & 1) << 31) | 0x37000000u
           | ((bit & 0x1F) << 19)
           | (((offset >> 2) & 0x3FFF) << 5) | (rt & 0x1F);
}

/* ── basic correctness ─────────────────────────────────────── */

TEST(emit_jump_basic)
{
    uint32_t buf[4] = {};
    void *addr = (void *)0xDEADBEEFCAFE0000ULL;
    emit_jump(buf, addr);

    ASSERT_HEX(buf[0], 0x58000050U);  // ldr x16, [pc, #8]
    ASSERT_HEX(buf[1], 0xD61F0200U);  // br x16
    ASSERT_HEX(*(uint64_t *)&buf[2], (uint64_t)addr);
    return 0;
}

TEST(emit_jump_zero)
{
    uint32_t buf[4] = {};
    emit_jump(buf, (void *)0);

    ASSERT_HEX(buf[0], 0x58000050U);
    ASSERT_HEX(buf[1], 0xD61F0200U);
    ASSERT_HEX(*(uint64_t *)&buf[2], 0);
    return 0;
}

TEST(emit_jump_max)
{
    uint32_t buf[4] = {};
    emit_jump(buf, (void *)0xFFFFFFFFFFFFFFFFULL);

    ASSERT_HEX(buf[0], 0x58000050U);
    ASSERT_HEX(buf[1], 0xD61F0200U);
    ASSERT_HEX(*(uint64_t *)&buf[2], 0xFFFFFFFFFFFFFFFFULL);
    return 0;
}

TEST(reloc_non_pc_relative)
{
    uint32_t dst;
    int ret = relocate_instruction(&dst, 0xD503201F, 0x1000, 0x5000);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(dst, 0xD503201F);
    return 0;
}

/* ── ADRP ──────────────────────────────────────────────────── */

TEST(reloc_adrp_forward)
{
    uint64_t orig_pc = 0x10000, new_pc = 0x80000;
    uint32_t insn = make_adrp(0, 0x3000);
    uint64_t target = decode_adrp(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_adrp(dst, new_pc), target);
    return 0;
}

TEST(reloc_adrp_backward)
{
    uint64_t orig_pc = 0x80000, new_pc = 0x10000;
    uint32_t insn = make_adrp(5, -0x5000);
    uint64_t target = decode_adrp(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_adrp(dst, new_pc), target);
    return 0;
}

TEST(reloc_adrp_preserves_rd)
{
    uint64_t orig_pc = 0x10000, new_pc = 0x80000;
    uint32_t insn = make_adrp(13, 0x3000);
    uint64_t target = decode_adrp(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_adrp(dst, new_pc), target);
    ASSERT_INT(dst & 0x1F, 13);
    return 0;
}

TEST(reloc_adrp_same_page)
{
    uint64_t orig_pc = 0x10000, new_pc = 0x10400;
    uint32_t insn = make_adrp(0, 0x0);
    uint64_t target = decode_adrp(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_adrp(dst, new_pc), target);
    return 0;
}

TEST(reloc_adrp_out_of_range)
{
    uint32_t insn = make_adrp(0, 0x3000);
    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, 0x1000, 0x200000000ULL);
    ASSERT_INT(ret, -1);
    return 0;
}

/* ── ADR ───────────────────────────────────────────────────── */

TEST(reloc_adr)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x101000;
    uint32_t insn = make_adr(3, 0x100);
    uint64_t target = decode_adr(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_adr(dst, new_pc), target);
    return 0;
}

TEST(reloc_adr_negative)
{
    uint64_t orig_pc = 0x101000, new_pc = 0x100000;
    uint32_t insn = make_adr(7, -0x200);
    uint64_t target = decode_adr(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_adr(dst, new_pc), target);
    ASSERT_INT(dst & 0x1F, 7);
    return 0;
}

TEST(reloc_adr_out_of_range)
{
    uint32_t insn = make_adr(0, 0x100);
    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, 0x100000, 0x300000);
    ASSERT_INT(ret, -1);
    return 0;
}

/* ── B / BL ────────────────────────────────────────────────── */

TEST(reloc_b)
{
    uint64_t orig_pc = 0x10000, new_pc = 0x50000;
    uint32_t insn = make_b(0x400);
    uint64_t target = decode_b(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_b(dst, new_pc), target);
    return 0;
}

TEST(reloc_b_negative)
{
    uint64_t orig_pc = 0x50000, new_pc = 0x10000;
    uint32_t insn = make_b(-0x400);
    uint64_t target = decode_b(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_b(dst, new_pc), target);
    return 0;
}

TEST(reloc_bl)
{
    uint64_t orig_pc = 0x10000, new_pc = 0x50000;
    uint32_t insn = make_bl(0x400);
    uint64_t target = decode_b(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_b(dst, new_pc), target);
    ASSERT_HEX(dst & 0xFC000000, 0x94000000u);
    return 0;
}

TEST(reloc_bl_negative)
{
    uint64_t orig_pc = 0x50000, new_pc = 0x10000;
    uint32_t insn = make_bl(-0x400);
    uint64_t target = decode_b(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_b(dst, new_pc), target);
    ASSERT_HEX(dst & 0xFC000000, 0x94000000u);
    return 0;
}

TEST(reloc_b_out_of_range)
{
    uint32_t insn = make_b(0x100);
    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, 0x10000, 0x10000000ULL);
    ASSERT_INT(ret, -1);
    return 0;
}

/* ── B.cond ────────────────────────────────────────────────── */

TEST(reloc_bcond)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x101000;
    uint32_t insn = make_bcond(0, 0x200);
    uint64_t target = decode_bcond(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_bcond(dst, new_pc), target);
    return 0;
}

TEST(reloc_bcond_preserves_cond)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x101000;
    uint32_t insn = make_bcond(1, 0x200); // NE
    uint64_t target = decode_bcond(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_bcond(dst, new_pc), target);
    ASSERT_INT(dst & 0xF, 1);
    return 0;
}

TEST(reloc_bcond_negative)
{
    uint64_t orig_pc = 0x101000, new_pc = 0x100000;
    uint32_t insn = make_bcond(2, -0x200);
    uint64_t target = decode_bcond(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_bcond(dst, new_pc), target);
    ASSERT_INT(dst & 0xF, 2);
    return 0;
}

TEST(reloc_bcond_out_of_range)
{
    uint32_t insn = make_bcond(0, 0x100);
    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, 0x100000, 0x200200);
    ASSERT_INT(ret, -1);
    return 0;
}

/* ── LDR literal ───────────────────────────────────────────── */

TEST(reloc_ldr_literal)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x101000;
    uint32_t insn = make_ldr_lit(2, 0x200);
    uint64_t target = decode_ldr_lit(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_ldr_lit(dst, new_pc), target);
    return 0;
}

TEST(reloc_ldr_literal_preserves_rt)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x101000;
    uint32_t insn = make_ldr_lit(20, 0x200);
    uint64_t target = decode_ldr_lit(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_ldr_lit(dst, new_pc), target);
    ASSERT_INT(dst & 0x1F, 20);
    return 0;
}

TEST(reloc_ldr_literal_negative)
{
    uint64_t orig_pc = 0x101000, new_pc = 0x100000;
    uint32_t insn = make_ldr_lit(5, -0x200);
    uint64_t target = decode_ldr_lit(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_ldr_lit(dst, new_pc), target);
    return 0;
}

TEST(reloc_ldr_literal_out_of_range)
{
    uint32_t insn = make_ldr_lit(0, 0x100);
    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, 0x100000, 0x200200);
    ASSERT_INT(ret, -1);
    return 0;
}

/* ── CBZ / CBNZ ───────────────────────────────────────────── */

TEST(reloc_cbz)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x101000;
    uint32_t insn = make_cbz(1, 0x200, 1);
    uint64_t target = decode_cbz(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_cbz(dst, new_pc), target);
    return 0;
}

TEST(reloc_cbnz)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x101000;
    uint32_t insn = make_cbnz(4, -0x100, 0);
    uint64_t target = decode_cbz(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_cbz(dst, new_pc), target);
    return 0;
}

TEST(reloc_cbz_preserves_rt_and_sf)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x101000;
    uint32_t insn = make_cbz(28, 0x200, 1);
    uint64_t target = decode_cbz(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_cbz(dst, new_pc), target);
    ASSERT_INT(dst & 0x1F, 28);
    ASSERT_HEX(dst & 0xFF000000, 0xB4000000u);
    return 0;
}

TEST(reloc_cbz_out_of_range)
{
    uint32_t insn = make_cbz(0, 0x100, 1);
    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, 0x100000, 0x200200);
    ASSERT_INT(ret, -1);
    return 0;
}

/* ── TBZ / TBNZ ───────────────────────────────────────────── */

TEST(reloc_tbz)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x100800;
    uint32_t insn = make_tbz(0, 7, 0x80);
    uint64_t target = decode_tbz(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_tbz(dst, new_pc), target);
    return 0;
}

TEST(reloc_tbnz)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x100800;
    uint32_t insn = make_tbnz(3, 31, -0x40);
    uint64_t target = decode_tbz(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_tbz(dst, new_pc), target);
    return 0;
}

TEST(reloc_tbz_high_bit)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x100800;
    uint32_t insn = make_tbz(2, 37, 0x80);
    uint64_t target = decode_tbz(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_tbz(dst, new_pc), target);
    ASSERT_HEX(dst & (1u << 31), 1u << 31); // b5 preserved
    ASSERT_INT((dst >> 19) & 0x1F, 5);       // b40 = 37 & 0x1F = 5
    return 0;
}

TEST(reloc_tbnz_high_bit)
{
    uint64_t orig_pc = 0x100000, new_pc = 0x100800;
    uint32_t insn = make_tbnz(6, 60, -0x40);
    uint64_t target = decode_tbz(insn, orig_pc);

    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, orig_pc, new_pc);
    ASSERT_INT(ret, 1);
    ASSERT_HEX(decode_tbz(dst, new_pc), target);
    ASSERT_HEX(dst & (1u << 31), 1u << 31); // b5 preserved (60 >= 32)
    ASSERT_INT((dst >> 19) & 0x1F, 28);      // b40 = 60 & 0x1F = 28
    return 0;
}

TEST(reloc_tbz_out_of_range)
{
    uint32_t insn = make_tbz(0, 0, 0x80);
    uint32_t dst;
    int ret = relocate_instruction(&dst, insn, 0x100000, 0x110000);
    ASSERT_INT(ret, -1);
    return 0;
}

/* ── integration: hook_inline / unhook_inline ──────────────── */

static void *alloc_func_page(int nops)
{
    long ps = getpagesize();
    void *page = mmap(NULL, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return NULL;
    uint32_t *insns = (uint32_t *)page;
    for (int i = 0; i < nops; i++) insns[i] = 0xD503201F;
    return page;
}

static void free_func_page(void *page)
{
    munmap(page, getpagesize());
}

TEST(hook_nop_insns)
{
    void *func = alloc_func_page(8);
    if (!func) return 1;

    void *replacement = (void *)0xCAFE0000ULL;
    void *original = NULL;

    int ret = hook_inline(func, replacement, &original);
    ASSERT_INT(ret, 0);

    uint32_t *target = (uint32_t *)func;
    ASSERT_HEX(target[0], 0x58000050U);  // ldr x16, [pc, #8]
    ASSERT_HEX(target[1], 0xD61F0200U);  // br x16
    ASSERT_HEX(*(uint64_t *)&target[2], (uint64_t)replacement);

    struct trampoline_header *hdr = (struct trampoline_header *)((uint8_t *)original - HEADER_SIZE);
    ASSERT_HEX((uint64_t)hdr->target, (uint64_t)func);
    ASSERT_HEX(hdr->orig_insns[0], 0xD503201F);
    ASSERT_HEX(hdr->orig_insns[3], 0xD503201F);

    uint32_t *code = (uint32_t *)original;
    ASSERT_HEX(code[0], 0xD503201F);
    ASSERT_HEX(code[3], 0xD503201F);

    ASSERT_HEX(code[4], 0x58000050U);  // ldr x16, [pc, #8]
    ASSERT_HEX(code[5], 0xD61F0200U);  // br x16
    ASSERT_HEX(*(uint64_t *)&code[6], (uint64_t)((uint8_t *)func + STOLEN_BYTES));

    free_func_page(func);
    munmap(hdr, hdr->mapped_size);
    return 0;
}

TEST(hook_and_unhook)
{
    void *func = alloc_func_page(8);
    if (!func) return 1;

    uint32_t saved[4];
    memcpy(saved, func, sizeof(saved));

    void *replacement = (void *)0xBEEF0000ULL;
    void *original = NULL;

    int ret = hook_inline(func, replacement, &original);
    ASSERT_INT(ret, 0);

    ret = unhook_inline(func, original);
    ASSERT_INT(ret, 0);

    ASSERT_HEX(((uint32_t *)func)[0], saved[0]);
    ASSERT_HEX(((uint32_t *)func)[1], saved[1]);
    ASSERT_HEX(((uint32_t *)func)[2], saved[2]);
    ASSERT_HEX(((uint32_t *)func)[3], saved[3]);

    free_func_page(func);
    return 0;
}

TEST(hook_unaligned_fails)
{
    void *func = alloc_func_page(8);
    if (!func) return 1;

    void *misaligned = (uint8_t *)func + 2;
    void *replacement = (void *)0xCAFE0000ULL;
    void *original = NULL;

    int ret = hook_inline(misaligned, replacement, &original);
    ASSERT_INT(ret, -1);

    free_func_page(func);
    return 0;
}

TEST(unhook_wrong_target_fails)
{
    void *func = alloc_func_page(8);
    if (!func) return 1;

    void *replacement = (void *)0xCAFE0000ULL;
    void *original = NULL;

    int ret = hook_inline(func, replacement, &original);
    ASSERT_INT(ret, 0);

    void *wrong_target = (uint8_t *)func + 4;
    ret = unhook_inline(wrong_target, original);
    ASSERT_INT(ret, -1);

    struct trampoline_header *hdr = (struct trampoline_header *)((uint8_t *)original - HEADER_SIZE);
    munmap(hdr, hdr->mapped_size);
    free_func_page(func);
    return 0;
}

TEST(hook_with_adrp)
{
    long ps = getpagesize();
    void *func = mmap(NULL, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (func == MAP_FAILED) return 1;

    uint32_t *insns = (uint32_t *)func;
    insns[0] = make_adrp(0, 0x3000);
    insns[1] = 0x91000400; // ADD X0, X0, #1
    insns[2] = 0xD503201F; // NOP
    insns[3] = 0xD503201F; // NOP
    for (int i = 4; i < 8; i++) insns[i] = 0xD503201F;

    uint64_t orig_adrp_target = decode_adrp(insns[0], (uint64_t)func);

    void *replacement = (void *)0xDEAD0000ULL;
    void *original = NULL;

    int ret = hook_inline(func, replacement, &original);
    ASSERT_INT(ret, 0);

    uint32_t *code = (uint32_t *)original;
    uint64_t new_adrp_target = decode_adrp(code[0], (uint64_t)code);
    ASSERT_HEX(new_adrp_target, orig_adrp_target);
    ASSERT_HEX(code[1], 0x91000400);

    struct trampoline_header *hdr = (struct trampoline_header *)((uint8_t *)original - HEADER_SIZE);
    munmap(hdr, hdr->mapped_size);
    munmap(func, ps);
    return 0;
}

/* ── near allocation tests ─────────────────────────────────── */

TEST(near_alloc_basic) {
    long ps = getpagesize();
    void *func = mmap(NULL, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (func == MAP_FAILED) return 1;

    void *near_page = alloc_rwx_near(ps, (unsigned long)func);
    ASSERT_TRUE(near_page != NULL);

    intptr_t delta = (intptr_t)near_page - (intptr_t)func;
    if (delta < 0) delta = -delta;
    ASSERT_TRUE(delta <= 128 * 1024 * 1024);

    munmap(near_page, ps);
    munmap(func, ps);
    return 0;
}

TEST(hook_with_tbz_near) {
    long ps = getpagesize();
    void *func = mmap(NULL, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (func == MAP_FAILED) return 1;

    uint32_t *insns = (uint32_t *)func;
    insns[0] = 0x36800000; // TBZ W0, #0, +8 (skip next insn)
    insns[1] = 0xD503201F; // NOP
    insns[2] = 0xD503201F; // NOP
    insns[3] = 0xD503201F; // NOP
    for (int i = 4; i < 8; i++) insns[i] = 0xD503201F;

    void *replacement = (void *)0xDEAD0000ULL;
    void *original = NULL;

    int ret = hook_inline(func, replacement, &original);
    ASSERT_INT(ret, 0);

    struct trampoline_header *hdr = (struct trampoline_header *)((uint8_t *)original - HEADER_SIZE);
    munmap(hdr, hdr->mapped_size);
    munmap(func, ps);
    return 0;
}

/* ── main ──────────────────────────────────────────────────── */

int main(void)
{
    printf("inline_hook tests:\n");

    RUN_TEST(emit_jump_basic);
    RUN_TEST(emit_jump_zero);
    RUN_TEST(emit_jump_max);
    RUN_TEST(reloc_non_pc_relative);

    RUN_TEST(reloc_adrp_forward);
    RUN_TEST(reloc_adrp_backward);
    RUN_TEST(reloc_adrp_preserves_rd);
    RUN_TEST(reloc_adrp_same_page);
    RUN_TEST(reloc_adrp_out_of_range);

    RUN_TEST(reloc_adr);
    RUN_TEST(reloc_adr_negative);
    RUN_TEST(reloc_adr_out_of_range);

    RUN_TEST(reloc_b);
    RUN_TEST(reloc_b_negative);
    RUN_TEST(reloc_bl);
    RUN_TEST(reloc_bl_negative);
    RUN_TEST(reloc_b_out_of_range);

    RUN_TEST(reloc_bcond);
    RUN_TEST(reloc_bcond_preserves_cond);
    RUN_TEST(reloc_bcond_negative);
    RUN_TEST(reloc_bcond_out_of_range);

    RUN_TEST(reloc_ldr_literal);
    RUN_TEST(reloc_ldr_literal_preserves_rt);
    RUN_TEST(reloc_ldr_literal_negative);
    RUN_TEST(reloc_ldr_literal_out_of_range);

    RUN_TEST(reloc_cbz);
    RUN_TEST(reloc_cbnz);
    RUN_TEST(reloc_cbz_preserves_rt_and_sf);
    RUN_TEST(reloc_cbz_out_of_range);

    RUN_TEST(reloc_tbz);
    RUN_TEST(reloc_tbnz);
    RUN_TEST(reloc_tbz_high_bit);
    RUN_TEST(reloc_tbnz_high_bit);
    RUN_TEST(reloc_tbz_out_of_range);

    RUN_TEST(hook_nop_insns);
    RUN_TEST(hook_and_unhook);
    RUN_TEST(hook_unaligned_fails);
    RUN_TEST(unhook_wrong_target_fails);
    RUN_TEST(hook_with_adrp);

    // TODO: enable after implementing alloc_rwx_near
    // RUN_TEST(near_alloc_basic);
    // RUN_TEST(hook_with_tbz_near);

    printf("\n%d/%d passed, %d failed\n",
           __tf_pass, __tf_total, __tf_fail);
    return __tf_fail ? 1 : 0;
}
