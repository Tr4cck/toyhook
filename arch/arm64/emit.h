#ifndef TOYHOOK_ARCH_ARM64_EMIT_H
#define TOYHOOK_ARCH_ARM64_EMIT_H

#include <stdint.h>

/*
 * ARM64 instruction emitters.
 *
 * Each function writes one or more instruction words to *buf and
 * returns a pointer to the next free word — enables chaining:
 *
 *   uint32_t *p = buf;
 *   p = emit_ldr_literal(p, 16, 8);
 *   p = emit_br(p, 16);
 */

/* single-instruction emitters */
uint32_t *emit_nop(uint32_t *buf);
uint32_t *emit_ret(uint32_t *buf);
uint32_t *emit_br(uint32_t *buf, unsigned rn);
uint32_t *emit_blr(uint32_t *buf, unsigned rn);
uint32_t *emit_b(uint32_t *buf, int64_t offset);
uint32_t *emit_bl(uint32_t *buf, int64_t offset);
uint32_t *emit_ldr_literal(uint32_t *buf, unsigned rt, int64_t offset);
uint32_t *emit_mov_reg(uint32_t *buf, unsigned rd, unsigned rn);
uint32_t *emit_add_imm(uint32_t *buf, unsigned rd, unsigned rn, unsigned imm);
uint32_t *emit_movz_w(uint32_t *buf, unsigned rd, unsigned imm16);
uint32_t *emit_stp(uint32_t *buf, unsigned rt1, unsigned rt2, unsigned rn, int imm);
uint32_t *emit_stp_pre(uint32_t *buf, unsigned rt1, unsigned rt2, unsigned rn, int imm);
uint32_t *emit_ldp_post(uint32_t *buf, unsigned rt1, unsigned rt2, unsigned rn, int imm);

/*
 * emit_jump — absolute 64-bit jump via LDR X16 + BR X16 + literal.
 * Writes 16 bytes (4 words): ldr x16, [pc, #8]; br x16; .quad addr
 */
uint32_t *emit_jump(uint32_t *buf, void *addr);

#endif
