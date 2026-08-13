/*
 * xemu CPU debug access - see xemu-dbg.h.
 *
 * Include set mirrors xemu-xbe.c (which compiles in this tree with these
 * exact headers) plus target/i386's cpu.h for CPUX86State.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/boards.h"
#include "system/hw_accel.h"
#include "system/runstate.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "exec/watchpoint.h"

#include "xemu-dbg.h"
#include "xemu-guestmem.h"

/* Handles the CONFIG_CAPSTONE guard itself and supplies stub constants when
 * capstone is absent, so this include needs no #ifdef around it. */
#include "disas/capstone.h"

bool xemu_dbg_have_disasm(void)
{
#ifdef CONFIG_CAPSTONE
    return true;
#else
    return false;
#endif
}

static CPUX86State *dbg_env(void)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return NULL;
    }
    cpu_synchronize_state(cs);
    return &X86_CPU(cs)->env;
}

void xemu_dbg_get_regs(XemuDbgRegs *regs)
{
    CPUX86State *env;

    memset(regs, 0, sizeof(*regs));

    env = dbg_env();
    if (env == NULL) {
        return;
    }

    regs->eax = env->regs[R_EAX];
    regs->ecx = env->regs[R_ECX];
    regs->edx = env->regs[R_EDX];
    regs->ebx = env->regs[R_EBX];
    regs->esp = env->regs[R_ESP];
    regs->ebp = env->regs[R_EBP];
    regs->esi = env->regs[R_ESI];
    regs->edi = env->regs[R_EDI];
    regs->eip = env->eip;
    /* eflags is split across the lazy-flags fields; cpu_compute_eflags()
     * folds them back into the architectural value. Reading env->eflags
     * directly returns a partially stale word. */
    regs->eflags = cpu_compute_eflags(env);
    regs->cs = env->segs[R_CS].selector;
    regs->ss = env->segs[R_SS].selector;
    regs->ds = env->segs[R_DS].selector;
    regs->es = env->segs[R_ES].selector;
    regs->fs = env->segs[R_FS].selector;
    regs->gs = env->segs[R_GS].selector;
    regs->cr0 = env->cr[0];
    regs->cr2 = env->cr[2];
    regs->cr3 = env->cr[3];
    regs->cr4 = env->cr[4];
    regs->valid = true;
}

bool xemu_dbg_set_reg(const char *name, uint32_t value)
{
    CPUX86State *env = dbg_env();
    if (env == NULL) {
        return false;
    }

    static const struct { const char *n; int idx; } gpr[] = {
        { "eax", R_EAX }, { "ecx", R_ECX }, { "edx", R_EDX }, { "ebx", R_EBX },
        { "esp", R_ESP }, { "ebp", R_EBP }, { "esi", R_ESI }, { "edi", R_EDI },
    };
    for (size_t i = 0; i < ARRAY_SIZE(gpr); i++) {
        if (strcasecmp(name, gpr[i].n) == 0) {
            env->regs[gpr[i].idx] = value;
            return true;
        }
    }
    if (strcasecmp(name, "eip") == 0) {
        env->eip = value;
        return true;
    }
    if (strcasecmp(name, "eflags") == 0) {
        /* cpu_load_eflags() is declared in target/i386/tcg/helper-tcg.h,
         * which is private to the TCG frontend. gdbstub.c assigns
         * env->eflags directly for exactly this case, so do the same. */
        env->eflags = value;
        return true;
    }
    return false;
}

int xemu_dbg_disasm(uint32_t addr, int count, XemuDbgInsn *out)
{
#ifdef CONFIG_CAPSTONE
    /*
     * Opened once and kept. cs_open()/cs_close() per call is not cheap, and
     * the UI's backward-scroll heuristic calls this in a loop.
     */
    static csh handle;
    static bool handle_ready;
    int produced = 0;

    if (count <= 0) {
        return 0;
    }
    if (!handle_ready) {
        if (cs_open(CS_ARCH_X86, CS_MODE_32, &handle) != CS_ERR_OK) {
            return 0;
        }
        handle_ready = true;
    }

    while (produced < count) {
        uint8_t buf[16];
        ssize_t got;
        cs_insn *insn = NULL;
        size_t n;

        memset(&out[produced], 0, sizeof(out[produced]));
        out[produced].addr = addr;

        /*
         * Read a whole instruction's worth. A short read is normal at the end
         * of a mapped page, so take whatever came back and let capstone
         * decide whether it is enough.
         */
        got = xemu_virt_read(addr, buf, sizeof(buf));
        if (got <= 0) {
            out[produced].len = 1;
            out[produced].valid = false;
            snprintf(out[produced].mnemonic, sizeof(out[produced].mnemonic),
                     "??");
            produced++;
            addr += 1;
            continue;
        }

        n = cs_disasm(handle, buf, (size_t)got, addr, 1, &insn);
        if (n == 0) {
            out[produced].len = 1;
            out[produced].bytes[0] = buf[0];
            out[produced].valid = false;
            snprintf(out[produced].mnemonic, sizeof(out[produced].mnemonic),
                     "(bad)");
            produced++;
            addr += 1;
            continue;
        }

        out[produced].len = (uint8_t)MIN(insn[0].size, 16);
        memcpy(out[produced].bytes, insn[0].bytes, out[produced].len);
        snprintf(out[produced].mnemonic, sizeof(out[produced].mnemonic), "%s",
                 insn[0].mnemonic);
        snprintf(out[produced].ops, sizeof(out[produced].ops), "%s",
                 insn[0].op_str);
        out[produced].valid = true;

        addr += insn[0].size ? insn[0].size : 1;
        cs_free(insn, n);
        produced++;
    }

    return produced;
#else
    (void)addr;
    (void)count;
    (void)out;
    return 0;
#endif
}

bool xemu_dbg_bp_insert(uint32_t addr)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return false;
    }
    return cpu_breakpoint_insert(cs, addr, BP_GDB, NULL) == 0;
}

bool xemu_dbg_bp_remove(uint32_t addr)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return false;
    }
    return cpu_breakpoint_remove(cs, addr, BP_GDB) == 0;
}

bool xemu_dbg_wp_insert(uint32_t addr, uint32_t len, int flags)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL || len == 0) {
        return false;
    }
    return cpu_watchpoint_insert(cs, addr, len, flags | BP_GDB, NULL) == 0;
}

bool xemu_dbg_wp_remove(uint32_t addr, uint32_t len, int flags)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return false;
    }
    return cpu_watchpoint_remove(cs, addr, len, flags | BP_GDB) == 0;
}

bool xemu_dbg_is_running(void)
{
    return runstate_is_running();
}

void xemu_dbg_pause(void)
{
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
}

void xemu_dbg_resume(void)
{
    if (!runstate_is_running()) {
        vm_start();
    }
}

void xemu_dbg_step(void)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return;
    }
    /*
     * Single step by asking the CPU for one instruction then stopping again.
     * cpu_single_step() sets the flag; the vm_start/vm_stop pair is what
     * actually advances it.
     */
    cpu_single_step(cs, SSTEP_ENABLE | SSTEP_NOIRQ | SSTEP_NOTIMER);
    if (!runstate_is_running()) {
        vm_start();
    }
}

/* ------------------------------------------------------------------------ */
/* Address space                                                            */
/* ------------------------------------------------------------------------ */

bool xemu_dbg_to_phys(uint32_t va, uint32_t *pa)
{
    uint64_t out = 0;
    if (xemu_virt_to_phys(va, &out) != 0) {
        return false;
    }
    *pa = (uint32_t)out;
    return true;
}

/*
 * Physical -> virtual by searching the page tables.
 *
 * There is no reverse mapping in hardware; a physical page can be mapped at
 * several virtual addresses or none. This walks the user and kernel windows
 * looking for the first virtual page that translates to the wanted physical
 * page, which is what the external viewer did. Linear, so it is only
 * acceptable for the occasional address-space flip, not per byte.
 */
bool xemu_dbg_to_virt(uint32_t pa, uint32_t *va)
{
    static const struct { uint32_t lo, hi; } windows[] = {
        { 0x00010000, 0x08000000 },     /* user space: heap + XBE */
        { 0x80000000, 0x88000000 },     /* kernel window */
        { 0xD0000000, 0xD8000000 },     /* second kernel window */
    };
    uint32_t want = pa & TARGET_PAGE_MASK;

    for (size_t w = 0; w < ARRAY_SIZE(windows); w++) {
        for (uint32_t p = windows[w].lo; p < windows[w].hi;
             p += TARGET_PAGE_SIZE) {
            uint64_t got = 0;
            if (xemu_virt_to_phys(p, &got) == 0 &&
                (uint32_t)(got & TARGET_PAGE_MASK) == want) {
                *va = p + (pa & ~TARGET_PAGE_MASK);
                return true;
            }
        }
    }
    return false;
}

ssize_t xemu_dbg_read_space(uint32_t addr, void *buf, size_t len, bool virt)
{
    return virt ? xemu_virt_read(addr, buf, len)
                : xemu_phys_read(addr, buf, len);
}

ssize_t xemu_dbg_write_space(uint32_t addr, const void *buf, size_t len,
                             bool virt)
{
    return virt ? xemu_virt_write(addr, buf, len)
                : xemu_phys_write(addr, buf, len);
}

/* ------------------------------------------------------------------------ */
/* Stepping                                                                 */
/* ------------------------------------------------------------------------ */

int xemu_dbg_insn_len(uint32_t addr)
{
    XemuDbgInsn insn;
    if (xemu_dbg_disasm(addr, 1, &insn) != 1 || !insn.valid) {
        return 0;
    }
    return insn.len;
}

void xemu_dbg_run_to(uint32_t addr)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return;
    }
    /* BP_GDB so it is distinguishable from a user breakpoint; removed by the
     * UI once hit. */
    cpu_breakpoint_insert(cs, addr, BP_GDB, NULL);
    if (!runstate_is_running()) {
        vm_start();
    }
}

void xemu_dbg_step_over(void)
{
    CPUX86State *env = dbg_env();
    XemuDbgInsn insn;
    uint32_t eip;

    if (env == NULL) {
        return;
    }
    eip = env->eip;

    if (xemu_dbg_disasm(eip, 1, &insn) != 1 || !insn.valid) {
        xemu_dbg_step();
        return;
    }

    /*
     * Only a call needs stepping over. rep-prefixed string ops are a loop in
     * a single instruction, so they get the same treatment - otherwise a
     * single step sits through the whole loop.
     */
    bool is_call = strncmp(insn.mnemonic, "call", 4) == 0;
    bool is_rep  = strncmp(insn.mnemonic, "rep", 3) == 0;

    if (!is_call && !is_rep) {
        xemu_dbg_step();
        return;
    }

    /*
     * Temporary breakpoint on the instruction after, then continue - the way
     * every debugger does it. A breakpoint inside the call still stops there,
     * and recursion re-entering this site stops early. Both are the standard
     * trade-off rather than bugs.
     */
    xemu_dbg_run_to(eip + insn.len);
}

const char *xemu_dbg_step_out(void)
{
    CPUX86State *env = dbg_env();
    uint32_t ret = 0;

    if (env == NULL) {
        return "no cpu";
    }
    if (runstate_is_running()) {
        return "not stopped";
    }

    uint32_t ebp = env->regs[R_EBP];
    if (ebp == 0 ||
        xemu_virt_read(ebp + 4, &ret, sizeof(ret)) != (ssize_t)sizeof(ret)) {
        return "could not read [ebp+4]";
    }
    if (ret < 0x1000) {
        /*
         * Taken from [ebp+4], which assumes a standard frame. Optimised or
         * frame-pointer-omitted code has no such frame, and without unwind
         * information there is no reliable alternative - so say so rather
         * than jump somewhere wrong.
         */
        return "no return address at [ebp+4] - this frame has no frame "
               "pointer, so step out cannot find the caller";
    }

    xemu_dbg_run_to(ret);
    return NULL;
}
