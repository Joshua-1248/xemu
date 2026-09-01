/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * xemu CPU debug access
 *
 * Everything here needs cpu.h (CPUX86State) or capstone, both of which are
 * only available to TARGET-SPECIFIC sources. ui/xui is compiled
 * target-agnostic, so the debugger UI cannot touch either directly - it calls
 * through this plain C API instead, and the implementation is built only with the optional debug-tools feature.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_DBG_H
#define XEMU_DBG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XemuDbgRegs {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eip, eflags;
    uint32_t cs, ss, ds, es, fs, gs;
    uint32_t cr0, cr2, cr3, cr4;
    bool valid;
} XemuDbgRegs;

typedef struct XemuDbgInsn {
    uint32_t addr;
    uint8_t  bytes[16];
    uint8_t  len;
    char     mnemonic[32];
    char     ops[160];
    bool     valid;      /* false when the address is unmapped or undecodable */
} XemuDbgInsn;

/* Feature-owned description of the latest guest-debug stop.  This lets the
 * scripting feature wait for breakpoint/watchpoint hits without reaching into
 * QEMU internals or requiring a remote GDB connection. */
enum {
    XEMU_DBG_STOP_NONE = 0,
    XEMU_DBG_STOP_BREAKPOINT = 1,
    XEMU_DBG_STOP_WATCHPOINT = 2,
    XEMU_DBG_STOP_STEP = 3,
    XEMU_DBG_STOP_DEBUG = 4,
};

typedef struct XemuDbgStopEvent {
    uint64_t sequence;
    uint32_t pc;
    uint32_t address;
    uint32_t length;
    int flags;
    int type;
    bool physical;
    bool valid;
} XemuDbgStopEvent;

/* True when capstone was compiled in. The window says so rather than
 * silently showing nothing. */
bool xemu_dbg_have_disasm(void);

/* Read the CPU register file. regs->valid is false when no cpu exists. */
void xemu_dbg_get_regs(XemuDbgRegs *regs);

/* Set one register by name ("eax", "eip", ...). Returns false if unknown. */
bool xemu_dbg_set_reg(const char *name, uint32_t value);

/*
 * Decode `count` instructions starting at guest VIRTUAL address `addr`.
 * Returns the number written. An unmapped or undecodable address yields an
 * entry with valid=false and len=1 so the caller can still advance.
 */
int xemu_dbg_disasm(uint32_t addr, int count, XemuDbgInsn *out);

/*
 * Execution breakpoints. The legacy helpers are guest-virtual. The *_space
 * variants also accept a physical RAM address when virt=false; Xemu arms all
 * currently mapped virtual aliases of that physical byte.
 */
bool xemu_dbg_bp_insert(uint32_t addr);
bool xemu_dbg_bp_remove(uint32_t addr);
bool xemu_dbg_bp_insert_space(uint32_t addr, bool virt);
bool xemu_dbg_bp_remove_space(uint32_t addr, bool virt);

/*
 * Data watchpoints. `flags` is BP_MEM_READ (1), BP_MEM_WRITE (2) or both (3).
 * Virtual watchpoints use QEMU's CPU watchpoint facility. Physical watchpoints
 * use Xemu's Xbox RAM access callback path, so every CPU virtual alias of
 * that RAM address is watched; host-side tools and device DMA are ignored.
 * They are not merely translated once to a VA.
 */
bool xemu_dbg_wp_insert(uint32_t addr, uint32_t len, int flags);
bool xemu_dbg_wp_remove(uint32_t addr, uint32_t len, int flags);
bool xemu_dbg_wp_insert_space(uint32_t addr, uint32_t len, int flags,
                              bool virt);
bool xemu_dbg_wp_remove_space(uint32_t addr, uint32_t len, int flags,
                              bool virt);

/*
 * Address space. The Xbox maps its XBE through page tables, so the same byte
 * has two addresses: the guest VIRTUAL one a cheat code uses, and the
 * PHYSICAL offset in the RAM block. The viewer can show either.
 */
ssize_t xemu_dbg_read_space(uint32_t addr, void *buf, size_t len, bool virt);
ssize_t xemu_dbg_write_space(uint32_t addr, const void *buf, size_t len,
                             bool virt);

/* Translate between the two spaces. false when the address is not mapped. */
bool xemu_dbg_to_phys(uint32_t va, uint32_t *pa);
bool xemu_dbg_to_virt(uint32_t pa, uint32_t *va);

/* Step over: a call is one step. Uses a temporary breakpoint on the next
 * instruction, exactly as the external debugger did. */
void xemu_dbg_step_over(void);

/*
 * Step out: run to the return address of the current frame, read from
 * [ebp+4]. Returns NULL on success, or an explanation when the frame has no
 * frame pointer (optimised code) and the caller cannot be found.
 */
const char *xemu_dbg_step_out(void);

/* Run to a specific address (temporary breakpoint + continue). */
void xemu_dbg_run_to(uint32_t addr);

/* Size in bytes of the instruction at addr, 0 if undecodable. */
int xemu_dbg_insn_len(uint32_t addr);

/* Run state. */
bool xemu_dbg_is_running(void);
void xemu_dbg_pause(void);
void xemu_dbg_resume(void);
void xemu_dbg_step(void);

/* Return the most recent guest-debug stop and a monotonically increasing
 * sequence number.  `valid` is false until the first EXCP_DEBUG stop. */
bool xemu_dbg_get_stop_event(XemuDbgStopEvent *event);

#ifdef __cplusplus
}
#endif

#endif
