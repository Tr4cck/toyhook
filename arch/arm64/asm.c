#include "asm.h"

#include <stdint.h>
#include "utils/log.h"

int relocate_instruction(uint32_t *dst, uint32_t insn,
                         unsigned long orig_pc, unsigned long new_pc)
{
    if ((insn & 0x9F000000) == 0x90000000) {
        uint32_t immlo = (insn >> 29) & 0x3;
        uint32_t immhi = (insn >> 5) & 0x7FFFF;
        int64_t imm = ((int64_t)((int32_t)((immhi << 2 | immlo) << 11)) >> 11) << 12;
        unsigned long target_page = (orig_pc + imm) & ~0xFFFUL;
        int64_t new_imm = (int64_t)(target_page - (new_pc & ~0xFFFUL)) >> 12;
        if (new_imm > 0xFFFFF || new_imm < -0x100000) {
            LOGE("ADRP out of range: target_page=0x%lx new_pc=0x%lx", target_page, new_pc);
            return -1;
        }
        *dst = (insn & 0x9F00001F) | (((new_imm >> 2) & 0x7FFFF) << 5) | ((new_imm & 0x3) << 29);
        return 1;
    }
    if ((insn & 0x9F000000) == 0x10000000) {
        uint32_t immlo = (insn >> 29) & 0x3;
        uint32_t immhi = (insn >> 5) & 0x7FFFF;
        int64_t imm = ((int64_t)(int32_t)((immhi << 2 | immlo) << 11)) >> 11;
        unsigned long target_addr = orig_pc + imm;
        int64_t new_imm = target_addr - new_pc;
        if (new_imm > 0xFFFFF || new_imm < -0x100000) {
            LOGE("ADR out of range: target_addr=0x%lx new_pc=0x%lx", target_addr, new_pc);
            return -1;
        }
        *dst = (insn & 0x9F00001F) | (((new_imm >> 2) & 0x7FFFF) << 5) | ((new_imm & 0x3) << 29);
        return 1;
    }
    if ((insn & 0x7C000000) == 0x14000000) {
        uint32_t imm26 = insn & 0x3FFFFFF;
        int64_t imm = ((int64_t)(int32_t)(imm26 << 6)) >> 4;
        unsigned long target_addr = orig_pc + imm;
        int64_t new_imm = target_addr - new_pc;
        if (new_imm > 0x1FFFFFF * 4 || new_imm < -0x2000000 * 4) {
            LOGE("B/BL out of range: target_addr=0x%lx new_pc=0x%lx", target_addr, new_pc);
            return -1;
        }
        *dst = (insn & 0xFC000000) | ((new_imm >> 2) & 0x3FFFFFF);
        return 1;
    }
    if ((insn & 0xFF000000) == 0x54000000) {
        uint32_t imm19 = (insn >> 5) & 0x7FFFF;
        int64_t imm = ((int64_t)(int32_t)(imm19 << 13)) >> 11;
        unsigned long target_addr = orig_pc + imm;
        int64_t new_imm = target_addr - new_pc;
        if (new_imm > 0x3FFFF * 4 || new_imm < -0x40000 * 4) {
            LOGE("B.cond out of range: target_addr=0x%lx new_pc=0x%lx", target_addr, new_pc);
            return -1;
        }
        *dst = (insn & 0xFF00001F) | ((new_imm >> 2) & 0x7FFFF) << 5;
        return 1;
    }
    if ((insn & 0x3C000000) == 0x18000000) {
        uint32_t imm19 = (insn >> 5) & 0x7FFFF;
        int64_t imm = ((int64_t)(int32_t)(imm19 << 13)) >> 11;
        unsigned long target_addr = orig_pc + imm;
        int64_t new_imm = target_addr - new_pc;
        if (new_imm > 0x3FFFF * 4 || new_imm < -0x40000 * 4) {
            LOGE("LDR literal out of range: target_addr=0x%lx new_pc=0x%lx", target_addr, new_pc);
            return -1;
        }
        *dst = (insn & 0xFF00001F) | ((new_imm >> 2) & 0x7FFFF) << 5;
        return 1;
    }
    if ((insn & 0xFF000000) == 0x34000000 || (insn & 0xFF000000) == 0xB4000000 ||
        (insn & 0xFF000000) == 0x35000000 || (insn & 0xFF000000) == 0xB5000000) {
        uint32_t imm19 = (insn >> 5) & 0x7FFFF;
        int64_t imm = ((int64_t)(int32_t)(imm19 << 13)) >> 11;
        unsigned long target_addr = orig_pc + imm;
        int64_t new_imm = target_addr - new_pc;
        if (new_imm > 0x3FFFF * 4 || new_imm < -0x40000 * 4) {
            LOGE("CBZ/CBNZ out of range: target_addr=0x%lx new_pc=0x%lx", target_addr, new_pc);
            return -1;
        }
        *dst = (insn & 0xFF00001F) | ((new_imm >> 2) & 0x7FFFF) << 5;
        return 1;
    }
    if ((insn & 0x7F000000) == 0x36000000 ||
        (insn & 0x7F000000) == 0x37000000) {
        uint32_t imm14 = (insn >> 5) & 0x3FFF;
        int64_t imm = ((int64_t)(int32_t)(imm14 << 18)) >> 16;
        unsigned long target_addr = orig_pc + imm;
        int64_t new_imm = target_addr - new_pc;
        if (new_imm > 0x1FFF * 4 || new_imm < -0x2000 * 4) {
            LOGE("TBZ/TBNZ out of range: target_addr=0x%lx new_pc=0x%lx", target_addr, new_pc);
            return -1;
        }
        *dst = (insn & 0xFFF8001F) | ((new_imm >> 2) & 0x3FFF) << 5;
        return 1;
    }
    *dst = insn;
    return 1;
}
