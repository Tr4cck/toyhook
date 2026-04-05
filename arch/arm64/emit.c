#include "emit.h"

/*
 * STP / LDP encoding (64-bit, opc=10):
 *
 *   [31:30] opc=10  [29:27] 101  [26] V=0
 *   [25:23] addressing: 010=offset  011=pre-index  001=post-index
 *   [22]    L=1 for load
 *   [21:15] imm7 (signed, scaled by 8)
 *   [14:10] Rt2  [9:5] Rn  [4:0] Rt1
 *
 * Bases:
 *   STP offset:    0xA9000000   STP pre:  0xA9800000
 *   LDP post:      0xA8C00000
 */

uint32_t *emit_nop(uint32_t *buf) {
    *buf = 0xD503201F;
    return buf + 1;
}

uint32_t *emit_ret(uint32_t *buf) {
    *buf = 0xD65F03C0;
    return buf + 1;
}

uint32_t *emit_br(uint32_t *buf, unsigned rn) {
    *buf = 0xD61F0000u | ((rn & 0x1F) << 5);
    return buf + 1;
}

uint32_t *emit_blr(uint32_t *buf, unsigned rn) {
    *buf = 0xD63F0000u | ((rn & 0x1F) << 5);
    return buf + 1;
}

uint32_t *emit_b(uint32_t *buf, int64_t offset) {
    *buf = 0x14000000u | ((offset >> 2) & 0x3FFFFFF);
    return buf + 1;
}

uint32_t *emit_bl(uint32_t *buf, int64_t offset) {
    *buf = 0x94000000u | ((offset >> 2) & 0x3FFFFFF);
    return buf + 1;
}

uint32_t *emit_ldr_literal(uint32_t *buf, unsigned rt, int64_t offset) {
    *buf = 0x58000000u | (((offset >> 2) & 0x7FFFF) << 5) | (rt & 0x1F);
    return buf + 1;
}

uint32_t *emit_mov_reg(uint32_t *buf, unsigned rd, unsigned rn) {
    *buf = 0xAA0003E0u | ((rn & 0x1F) << 16) | (rd & 0x1F);
    return buf + 1;
}

uint32_t *emit_add_imm(uint32_t *buf, unsigned rd, unsigned rn, unsigned imm) {
    *buf = 0x91000000u | ((imm & 0xFFF) << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F);
    return buf + 1;
}

uint32_t *emit_movz_w(uint32_t *buf, unsigned rd, unsigned imm16) {
    *buf = 0x52800000u | ((imm16 & 0xFFFF) << 5) | (rd & 0x1F);
    return buf + 1;
}

uint32_t *emit_stp(uint32_t *buf, unsigned rt1, unsigned rt2, unsigned rn, int imm) {
    int imm7 = imm / 8;
    *buf = 0xA9000000u | ((imm7 & 0x7F) << 15) | ((rt2 & 0x1F) << 10)
                           | ((rn & 0x1F) << 5)  | (rt1 & 0x1F);
    return buf + 1;
}

uint32_t *emit_stp_pre(uint32_t *buf, unsigned rt1, unsigned rt2, unsigned rn, int imm) {
    int imm7 = imm / 8;
    *buf = 0xA9800000u | ((imm7 & 0x7F) << 15) | ((rt2 & 0x1F) << 10)
                           | ((rn & 0x1F) << 5)  | (rt1 & 0x1F);
    return buf + 1;
}

uint32_t *emit_ldp_post(uint32_t *buf, unsigned rt1, unsigned rt2, unsigned rn, int imm) {
    int imm7 = imm / 8;
    *buf = 0xA8C00000u | ((imm7 & 0x7F) << 15) | ((rt2 & 0x1F) << 10)
                           | ((rn & 0x1F) << 5)  | (rt1 & 0x1F);
    return buf + 1;
}

uint32_t *emit_jump(uint32_t *buf, void *addr) {
    buf = emit_ldr_literal(buf, 16, 8);
    buf = emit_br(buf, 16);
    *(uint64_t *)buf = (uint64_t)addr;
    return buf + 2;
}
