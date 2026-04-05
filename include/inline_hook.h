#ifndef TOYHOOK_INLINE_HOOK_H
#define TOYHOOK_INLINE_HOOK_H

#include <stdint.h>

/*
 * hook_inline - redirect a function by patching its entry point.
 *
 * @target       address of the function to hook (must be executable, 4-byte aligned)
 * @replacement  your hook function (same signature as target)
 * @original     receives a pointer to the trampoline (call this to invoke original)
 *
 * Returns 0 on success, -1 on failure.
 *
 * How it works:
 *   1. Saves the first 4 instructions (16 bytes) of @target
 *   2. Relocates them into a trampoline, fixing up PC-relative instructions
 *   3. Appends a jump back to target+16 at the end of the trampoline
 *   4. Overwrites target's first 16 bytes with: LDR X16, #8; BR X16; <replacement_addr>
 *
 * After hooking:
 *   - Calls to @target jump to @replacement
 *   - Calling *@original runs the saved instructions, then jumps back to target+16
 *
 * WARNING: Not thread-safe. The patch is not atomic — another thread executing
 * the first 4 instructions during patching will see undefined behavior.
 */
int hook_inline(void *target,
                void *replacement,
                void **original);

/*
 * unhook_inline - restore a previously hooked function.
 *
 * @target       the same address passed to hook_inline
 * @trampoline   the value written to *original by hook_inline
 *
 * Restores the original instructions and frees the trampoline.
 * Returns 0 on success, -1 on failure.
 */
int unhook_inline(void *target, void *trampoline);

/*
 * hook_inline_get_insns - retrieve the original instructions saved in the
 * trampoline header.
 *
 * @original   the value written to *original by hook_inline
 * @out        receives up to 4 original instruction words
 *
 * Returns the number of instructions saved (always 4 currently), or -1 on
 * error (e.g. @original is NULL or the header magic doesn't match).
 */
int hook_inline_get_insns(void *original, uint32_t out[4]);

#endif
