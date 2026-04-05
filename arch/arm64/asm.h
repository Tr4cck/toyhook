#ifndef TOYHOOK_ARCH_ARM64_ASM_H
#define TOYHOOK_ARCH_ARM64_ASM_H

#include <stdint.h>

int relocate_instruction(uint32_t *dst, uint32_t insn,
                         unsigned long orig_pc, unsigned long new_pc);

#endif
