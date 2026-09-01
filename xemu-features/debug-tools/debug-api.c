/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * xemu custom CPU debug access - see debug-api.h.
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
#include "system/cpus.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "exec/watchpoint.h"
#include "exec/cpu-interrupt.h"
#include "qemu/main-loop.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "exec/gdbstub.h"
#include "qapi/error.h"
#include "gdbstub/internals.h"

#include "xemu-features/debug-tools/debug-api.h"
#include "xemu-features/shared/guest-memory.h"

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

/*
 * QEMU's BP_GDB / single-step machinery always routes EXCP_DEBUG through
 * gdb_set_stop_cpu(), even when Xemu's built-in debugger is the only debugger
 * in use.  The network GDB server normally creates the process table that
 * gdb_set_stop_cpu() expects, but our in-process debugger deliberately does
 * not open a GDB socket.
 *
 * Keep this compatibility entirely inside the optional debug-tools feature:
 * lazily initialize QEMU's existing GDB bookkeeping in its documented
 * "none" mode.  That mode creates no listener/chardev for a remote debugger;
 * it only establishes the internal process table and leaves the GDB state
 * inactive.  If a real GDB server was already started, leave it alone.
 */
static bool dbg_prepare_guest_debug(void)
{
    if (gdbserver_state.processes != NULL &&
        gdbserver_state.process_num > 0) {
        return true;
    }

    Error *local_err = NULL;
    if (!gdbserver_start("none", &local_err)) {
        if (local_err != NULL) {
            error_report_err(local_err);
        }
        return false;
    }

    return gdbserver_state.processes != NULL &&
           gdbserver_state.process_num > 0;
}


/* ------------------------------------------------------------------------ */
/* Physical debugger traps                                                   */
/* ------------------------------------------------------------------------ */

/*
 * QEMU's ordinary cpu_watchpoint_* API is intentionally virtual-addressed.
 * Xemu, however, already has an Xbox-only RAM access callback path used by
 * the UMA/GPU synchronisation code.  That callback is keyed by RAM address,
 * so it is exactly the right primitive for a *real* physical CPU watchpoint:
 * accesses through any virtual alias all resolve to the same RAM range.
 * Host-side debugger/cheat reads and device DMA are deliberately ignored so
 * only guest CPU activity can stop execution.
 */
typedef struct XemuDbgPhysWatchpoint {
    uint32_t addr;
    uint32_t len;
    int flags;
    MemAccessCallback *cb;
    struct XemuDbgPhysWatchpoint *next;
} XemuDbgPhysWatchpoint;

#define XEMU_DBG_MAX_PHYS_BP_ALIASES 64

typedef struct XemuDbgPhysBreakpoint {
    uint32_t addr;
    uint32_t aliases[XEMU_DBG_MAX_PHYS_BP_ALIASES];
    bool owned[XEMU_DBG_MAX_PHYS_BP_ALIASES];
    size_t alias_count;
    struct XemuDbgPhysBreakpoint *next;
} XemuDbgPhysBreakpoint;

static XemuDbgPhysWatchpoint *dbg_phys_watchpoints;
static XemuDbgPhysBreakpoint *dbg_phys_breakpoints;

/* Physical RAM callbacks do not populate CPUState::watchpoint_hit, because
 * they intentionally sit below the guest virtual-address watchpoint layer.
 * Preserve the hit details here so script/debug clients can still receive a
 * precise event. */
static bool dbg_phys_hit_pending;
static uint32_t dbg_phys_hit_addr;
static uint32_t dbg_phys_hit_len;
static int dbg_phys_hit_flags;

static uint64_t dbg_stop_sequence;
static bool dbg_stop_seen_running = true;
static XemuDbgStopEvent dbg_last_stop_event;

/* ------------------------------------------------------------------------ */
/* Feature-owned stepping state                                              */
/* ------------------------------------------------------------------------ */

/*
 * QEMU's low-level single-step bit is persistent: cpu_single_step(...,
 * SSTEP_ENABLE) stays armed until somebody explicitly clears it.  A network
 * GDB session normally owns that lifecycle.  Our in-process debugger does
 * not, so leaving the bit armed makes every later Resume/Step Over/Step Out
 * immediately fall back into RUN_STATE_DEBUG and look permanently paused.
 *
 * A second, related problem occurs when execution is stopped *on* one of our
 * BP_GDB breakpoints.  Resuming without first stepping past that address just
 * hits the same breakpoint again before the instruction can execute.
 *
 * Keep the whole fix in debug-tools: temporarily remove the breakpoint at the
 * current EIP, execute one instruction, re-arm it, then either stay stopped
 * (Step Into) or continue toward a temporary target (Resume/Step Over/Out).
 */
static bool dbg_step_armed;
static bool dbg_step_stop_pending;
static bool dbg_auto_continue_after_step;
static bool dbg_rearm_bp;
static uint32_t dbg_rearm_bp_addr;

static bool dbg_temp_bp_active;
static bool dbg_temp_bp_owned;
static uint32_t dbg_temp_bp_addr;
static CPUBreakpoint *dbg_temp_bp_ref;

static void dbg_remove_temp_breakpoint(CPUState *cs)
{
    if (dbg_temp_bp_active && dbg_temp_bp_owned &&
        dbg_temp_bp_ref != NULL && cs != NULL) {
        cpu_breakpoint_remove_by_ref(cs, dbg_temp_bp_ref);
    }
    dbg_temp_bp_active = false;
    dbg_temp_bp_owned = false;
    dbg_temp_bp_addr = 0;
    dbg_temp_bp_ref = NULL;
}

static bool dbg_set_temp_breakpoint(CPUState *cs, uint32_t addr)
{
    dbg_remove_temp_breakpoint(cs);
    if (cs == NULL) {
        return false;
    }

    dbg_temp_bp_addr = addr;
    dbg_temp_bp_active = true;
    if (cpu_breakpoint_test(cs, addr, BP_GDB)) {
        /* A user/physical-alias breakpoint already owns this address. */
        dbg_temp_bp_owned = false;
        dbg_temp_bp_ref = NULL;
        return true;
    }

    if (cpu_breakpoint_insert(cs, addr, BP_GDB, &dbg_temp_bp_ref) != 0) {
        dbg_temp_bp_active = false;
        dbg_temp_bp_addr = 0;
        dbg_temp_bp_ref = NULL;
        return false;
    }
    dbg_temp_bp_owned = true;
    return true;
}

static uint32_t dbg_get_eip(CPUState *cs)
{
    if (cs == NULL) {
        return 0;
    }
    cpu_synchronize_state(cs);
    return X86_CPU(cs)->env.eip;
}

static bool dbg_suspend_breakpoint_at_pc(CPUState *cs, uint32_t eip)
{
    if (cs == NULL || !cpu_breakpoint_test(cs, eip, BP_GDB)) {
        return false;
    }
    if (cpu_breakpoint_remove(cs, eip, BP_GDB) != 0) {
        return false;
    }
    dbg_rearm_bp = true;
    dbg_rearm_bp_addr = eip;
    return true;
}

static void dbg_rearm_suspended_breakpoint(CPUState *cs)
{
    if (!dbg_rearm_bp || cs == NULL) {
        return;
    }
    if (!cpu_breakpoint_test(cs, dbg_rearm_bp_addr, BP_GDB)) {
        cpu_breakpoint_insert(cs, dbg_rearm_bp_addr, BP_GDB, NULL);
    }
    dbg_rearm_bp = false;
    dbg_rearm_bp_addr = 0;
}

static void dbg_start_vm(bool step_pending)
{
    /* Match QEMU's own gdb_continue_partial() startup contract so accelerators
     * are told about pending single-step state before vCPUs resume. */
    dbg_stop_seen_running = true;
    if (!vm_prepare_start(step_pending)) {
        resume_all_vcpus();
    }
}

static bool dbg_start_one_instruction(CPUState *cs, bool continue_after)
{
    if (cs == NULL || !dbg_prepare_guest_debug()) {
        return false;
    }

    /* Never inherit a stale single-step request from an earlier stop. */
    if (cs->singlestep_enabled) {
        cpu_single_step(cs, 0);
    }

    const uint32_t eip = dbg_get_eip(cs);
    dbg_suspend_breakpoint_at_pc(cs, eip);

    dbg_auto_continue_after_step = continue_after;
    dbg_step_armed = true;
    cpu_single_step(cs, SSTEP_ENABLE | SSTEP_NOIRQ | SSTEP_NOTIMER);
    if (!runstate_is_running()) {
        dbg_start_vm(true);
    }
    return true;
}

/* Called from xemu_dbg_is_running() and before every explicit debugger action.
 * It performs the bit of lifecycle work that a remote GDB session normally
 * performs after EXCP_DEBUG. */
static void dbg_finish_stopped_step(void)
{
    CPUState *cs;
    uint32_t eip;
    bool hit_temp = false;
    bool auto_continue = false;

    if (runstate_is_running()) {
        return;
    }

    cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return;
    }

    eip = dbg_get_eip(cs);
    if (dbg_temp_bp_active && eip == dbg_temp_bp_addr) {
        hit_temp = true;
        dbg_remove_temp_breakpoint(cs);
    }

    if (!dbg_step_armed) {
        return;
    }

    if (cs->singlestep_enabled) {
        cpu_single_step(cs, 0);
    }
    if (runstate_get() == RUN_STATE_DEBUG) {
        dbg_step_stop_pending = true;
    }
    dbg_step_armed = false;
    dbg_rearm_suspended_breakpoint(cs);

    /* Only auto-resume an EXCP_DEBUG stop caused while stepping past a
     * breakpoint.  A manual Pause must always remain paused.  Reaching the
     * requested temporary target also completes the operation immediately. */
    auto_continue = dbg_auto_continue_after_step &&
                    runstate_get() == RUN_STATE_DEBUG && !hit_temp;
    dbg_auto_continue_after_step = false;

    if (auto_continue) {
        dbg_step_stop_pending = false;
        dbg_stop_seen_running = true;
        vm_start();
    }
}

/* Debugger-owned memory reads/writes (memory viewer, disassembly refresh,
 * register helpers) must not trip a physical watchpoint themselves.  Keep
 * this thread-local so a UI refresh cannot mask a simultaneous guest access
 * on the vCPU thread. */
static _Thread_local unsigned dbg_mem_access_suppress;

static ssize_t dbg_virt_read(uint64_t addr, void *buf, size_t len)
{
    ssize_t ret;
    dbg_mem_access_suppress++;
    ret = xemu_virt_read(addr, buf, len);
    dbg_mem_access_suppress--;
    return ret;
}

static ssize_t dbg_phys_read(uint64_t addr, void *buf, size_t len)
{
    ssize_t ret;
    dbg_mem_access_suppress++;
    ret = xemu_phys_read(addr, buf, len);
    dbg_mem_access_suppress--;
    return ret;
}

static ssize_t dbg_virt_write(uint64_t addr, const void *buf, size_t len)
{
    ssize_t ret;
    dbg_mem_access_suppress++;
    ret = xemu_virt_write(addr, buf, len);
    dbg_mem_access_suppress--;
    return ret;
}

static ssize_t dbg_phys_write(uint64_t addr, const void *buf, size_t len)
{
    ssize_t ret;
    dbg_mem_access_suppress++;
    ret = xemu_phys_write(addr, buf, len);
    dbg_mem_access_suppress--;
    return ret;
}

static void dbg_phys_watchpoint_hit(void *opaque, MemoryRegion *mr,
                                    hwaddr addr, hwaddr len, bool write)
{
    XemuDbgPhysWatchpoint *wp = opaque;
    CPUState *cs;
    int access = write ? BP_MEM_WRITE : BP_MEM_READ;

    (void)mr;
    (void)addr;
    (void)len;

    if (dbg_mem_access_suppress || !runstate_is_running() ||
        (wp->flags & access) == 0) {
        return;
    }

    cs = qemu_get_cpu(0);
    if (cs == NULL || !qemu_cpu_is_self(cs)) {
        return;
    }

    dbg_phys_hit_pending = true;
    dbg_phys_hit_addr = wp->addr;
    dbg_phys_hit_len = wp->len;
    dbg_phys_hit_flags = access;

    /* Match the normal TCG watchpoint stop path: request a guest-debug
     * interrupt. cpu_handle_guest_debug() turns EXCP_DEBUG into Xemu's normal
     * paused/debug state. */
    const bool need_bql = !bql_locked();
    if (need_bql) {
        bql_lock();
    }
    cpu_interrupt(cs, CPU_INTERRUPT_DEBUG);
    if (need_bql) {
        bql_unlock();
    }
}

static size_t dbg_phys_find_aliases(uint32_t pa, uint32_t *out, size_t cap)
{
    static const struct { uint32_t lo, hi; } windows[] = {
        { 0x00010000, 0x08000000 },
        { 0x80000000, 0x88000000 },
        { 0xD0000000, 0xD8000000 },
    };
    const uint32_t want_page = pa & TARGET_PAGE_MASK;
    const uint32_t page_off = pa & ~TARGET_PAGE_MASK;
    size_t count = 0;

    xemu_guestmem_invalidate_cache();
    dbg_mem_access_suppress++;
    for (size_t w = 0; w < ARRAY_SIZE(windows); ++w) {
        for (uint32_t va = windows[w].lo; va < windows[w].hi;
             va += TARGET_PAGE_SIZE) {
            uint64_t got = 0;
            if (xemu_virt_to_phys(va, &got) != 0 ||
                (uint32_t)(got & TARGET_PAGE_MASK) != want_page) {
                continue;
            }
            uint32_t alias = va + page_off;
            bool duplicate = false;
            for (size_t i = 0; i < count; ++i) {
                if (out[i] == alias) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate && count < cap) {
                out[count++] = alias;
            }
        }
    }
    dbg_mem_access_suppress--;
    return count;
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
     * Keep both the Capstone handle and its reusable instruction object for
     * the lifetime of xemu. cs_disasm(..., 1, &ptr) allocates/free()s an
     * instruction for every line; the debugger's backward-scroll heuristic
     * can decode hundreds of candidates in a single UI action.
     */
    static csh handle;
    static cs_insn *insn;
    static bool handle_ready;
    int produced = 0;

    if (count <= 0) {
        return 0;
    }
    if (!handle_ready) {
        if (cs_open(CS_ARCH_X86, CS_MODE_32, &handle) != CS_ERR_OK) {
            return 0;
        }
        insn = cs_malloc(handle);
        if (insn == NULL) {
            cs_close(&handle);
            return 0;
        }
        handle_ready = true;
    }

    while (produced < count) {
        uint8_t buf[16];
        ssize_t got;

        memset(&out[produced], 0, sizeof(out[produced]));
        out[produced].addr = addr;

        got = dbg_virt_read(addr, buf, sizeof(buf));
        if (got <= 0) {
            out[produced].len = 1;
            out[produced].valid = false;
            snprintf(out[produced].mnemonic, sizeof(out[produced].mnemonic),
                     "??");
            produced++;
            addr += 1;
            continue;
        }

        const uint8_t *code = buf;
        size_t size = (size_t)got;
        uint64_t pc = addr;
        if (!cs_disasm_iter(handle, &code, &size, &pc, insn)) {
            out[produced].len = 1;
            out[produced].bytes[0] = buf[0];
            out[produced].valid = false;
            snprintf(out[produced].mnemonic, sizeof(out[produced].mnemonic),
                     "(bad)");
            produced++;
            addr += 1;
            continue;
        }

        out[produced].len = (uint8_t)MIN(insn->size, 16);
        memcpy(out[produced].bytes, insn->bytes, out[produced].len);
        snprintf(out[produced].mnemonic, sizeof(out[produced].mnemonic), "%s",
                 insn->mnemonic);
        snprintf(out[produced].ops, sizeof(out[produced].ops), "%s",
                 insn->op_str);
        out[produced].valid = true;

        addr += insn->size ? insn->size : 1;
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

bool xemu_dbg_bp_insert_space(uint32_t addr, bool virt)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL || !dbg_prepare_guest_debug()) {
        return false;
    }
    if (virt) {
        return cpu_breakpoint_insert(cs, addr, BP_GDB, NULL) == 0;
    }

    if ((uint64_t)addr >= xemu_guest_ram_size()) {
        return false;
    }
    for (XemuDbgPhysBreakpoint *it = dbg_phys_breakpoints; it; it = it->next) {
        if (it->addr == addr) {
            return false;
        }
    }

    XemuDbgPhysBreakpoint *bp = g_new0(XemuDbgPhysBreakpoint, 1);
    bp->addr = addr;
    bp->alias_count = dbg_phys_find_aliases(addr, bp->aliases,
                                             ARRAY_SIZE(bp->aliases));
    if (bp->alias_count == 0) {
        g_free(bp);
        return false;
    }

    size_t armed = 0;
    for (; armed < bp->alias_count; ++armed) {
        if (cpu_breakpoint_test(cs, bp->aliases[armed], BP_GDB)) {
            bp->owned[armed] = false;
            continue;
        }
        if (cpu_breakpoint_insert(cs, bp->aliases[armed], BP_GDB, NULL) != 0) {
            break;
        }
        bp->owned[armed] = true;
    }
    if (armed != bp->alias_count) {
        while (armed > 0) {
            --armed;
            if (bp->owned[armed]) {
                cpu_breakpoint_remove(cs, bp->aliases[armed], BP_GDB);
            }
        }
        g_free(bp);
        return false;
    }

    bp->next = dbg_phys_breakpoints;
    dbg_phys_breakpoints = bp;
    return true;
}

bool xemu_dbg_bp_remove_space(uint32_t addr, bool virt)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return false;
    }
    if (virt) {
        return cpu_breakpoint_remove(cs, addr, BP_GDB) == 0;
    }

    XemuDbgPhysBreakpoint **link = &dbg_phys_breakpoints;
    while (*link) {
        XemuDbgPhysBreakpoint *bp = *link;
        if (bp->addr != addr) {
            link = &bp->next;
            continue;
        }
        bool ok = true;
        for (size_t i = 0; i < bp->alias_count; ++i) {
            if (bp->owned[i] &&
                cpu_breakpoint_remove(cs, bp->aliases[i], BP_GDB) != 0) {
                ok = false;
            }
        }
        *link = bp->next;
        g_free(bp);
        return ok;
    }
    return false;
}

bool xemu_dbg_bp_insert(uint32_t addr)
{
    return xemu_dbg_bp_insert_space(addr, true);
}

bool xemu_dbg_bp_remove(uint32_t addr)
{
    return xemu_dbg_bp_remove_space(addr, true);
}

bool xemu_dbg_wp_insert_space(uint32_t addr, uint32_t len, int flags,
                              bool virt)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL || len == 0 || (flags & BP_MEM_ACCESS) == 0 ||
        !dbg_prepare_guest_debug()) {
        return false;
    }
    flags &= BP_MEM_ACCESS;

    if (virt) {
        return cpu_watchpoint_insert(cs, addr, len, flags | BP_GDB, NULL) == 0;
    }

    const uint64_t ram_size = xemu_guest_ram_size();
    if ((uint64_t)addr >= ram_size || (uint64_t)len > ram_size - addr) {
        return false;
    }
    for (XemuDbgPhysWatchpoint *it = dbg_phys_watchpoints; it; it = it->next) {
        if (it->addr == addr && it->len == len && it->flags == flags) {
            return false;
        }
    }

    MemoryRegionSection sec = memory_region_find(get_system_memory(), addr, len);
    if (sec.mr == NULL || !memory_region_is_ram(sec.mr) ||
        sec.offset_within_address_space > addr) {
        return false;
    }

    XemuDbgPhysWatchpoint *wp = g_new0(XemuDbgPhysWatchpoint, 1);
    wp->addr = addr;
    wp->len = len;
    wp->flags = flags;
    hwaddr offset = sec.offset_within_region +
                    ((hwaddr)addr - sec.offset_within_address_space);
    wp->cb = mem_access_callback_insert(cs, sec.mr, offset, len,
                                        dbg_phys_watchpoint_hit, wp);
    if (wp->cb == NULL) {
        g_free(wp);
        return false;
    }
    wp->next = dbg_phys_watchpoints;
    dbg_phys_watchpoints = wp;
    return true;
}

bool xemu_dbg_wp_remove_space(uint32_t addr, uint32_t len, int flags,
                              bool virt)
{
    CPUState *cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return false;
    }
    flags &= BP_MEM_ACCESS;
    if (virt) {
        return cpu_watchpoint_remove(cs, addr, len, flags | BP_GDB) == 0;
    }

    XemuDbgPhysWatchpoint **link = &dbg_phys_watchpoints;
    while (*link) {
        XemuDbgPhysWatchpoint *wp = *link;
        if (wp->addr != addr || wp->len != len || wp->flags != flags) {
            link = &wp->next;
            continue;
        }
        *link = wp->next;
        mem_access_callback_remove_by_ref(cs, wp->cb);
        g_free(wp);
        return true;
    }
    return false;
}

bool xemu_dbg_wp_insert(uint32_t addr, uint32_t len, int flags)
{
    return xemu_dbg_wp_insert_space(addr, len, flags, true);
}

bool xemu_dbg_wp_remove(uint32_t addr, uint32_t len, int flags)
{
    return xemu_dbg_wp_remove_space(addr, len, flags, true);
}

bool xemu_dbg_is_running(void)
{
    /* Complete any feature-owned single-step lifecycle before reporting the
     * state to the UI.  This may auto-resume when we were only stepping off a
     * breakpoint as part of Continue/Step Over/Step Out. */
    dbg_finish_stopped_step();
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
    CPUState *cs;

    dbg_finish_stopped_step();
    if (runstate_is_running()) {
        return;
    }

    cs = qemu_get_cpu(0);
    if (cs == NULL || !dbg_prepare_guest_debug()) {
        return;
    }

    /* If the CPU is parked exactly on a breakpoint, execute that one
     * instruction with the breakpoint temporarily removed, re-arm it, then
     * continue.  Otherwise Resume would immediately hit the same address. */
    if (cpu_breakpoint_test(cs, dbg_get_eip(cs), BP_GDB)) {
        dbg_start_one_instruction(cs, true);
        return;
    }

    if (cs->singlestep_enabled) {
        cpu_single_step(cs, 0);
    }
    dbg_stop_seen_running = true;
    vm_start();
}

void xemu_dbg_step(void)
{
    CPUState *cs;

    dbg_finish_stopped_step();
    if (runstate_is_running()) {
        return;
    }

    cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return;
    }

    /* A new Step Into supersedes any unfinished temporary run-to target. */
    dbg_remove_temp_breakpoint(cs);
    dbg_start_one_instruction(cs, false);
}

bool xemu_dbg_get_stop_event(XemuDbgStopEvent *event)
{
    CPUState *cs;
    bool running;

    if (event == NULL) {
        return false;
    }

    /* Finish feature-owned single-step bookkeeping first.  If that operation
     * intentionally auto-continues, there is no externally visible stop yet. */
    dbg_finish_stopped_step();
    running = runstate_is_running();
    if (running) {
        dbg_stop_seen_running = true;
        *event = dbg_last_stop_event;
        return event->valid;
    }

    if (dbg_stop_seen_running && runstate_get() == RUN_STATE_DEBUG) {
        memset(&dbg_last_stop_event, 0, sizeof(dbg_last_stop_event));
        dbg_last_stop_event.sequence = ++dbg_stop_sequence;
        dbg_last_stop_event.valid = true;

        cs = qemu_get_cpu(0);
        if (cs != NULL) {
            dbg_last_stop_event.pc = dbg_get_eip(cs);

            if (dbg_phys_hit_pending) {
                dbg_last_stop_event.type = XEMU_DBG_STOP_WATCHPOINT;
                dbg_last_stop_event.address = dbg_phys_hit_addr;
                dbg_last_stop_event.length = dbg_phys_hit_len;
                dbg_last_stop_event.flags = dbg_phys_hit_flags;
                dbg_last_stop_event.physical = true;
                dbg_phys_hit_pending = false;
            } else if (cs->watchpoint_hit != NULL) {
                CPUWatchpoint *wp = cs->watchpoint_hit;
                dbg_last_stop_event.type = XEMU_DBG_STOP_WATCHPOINT;
                dbg_last_stop_event.address = (uint32_t)wp->hitaddr;
                dbg_last_stop_event.length = (uint32_t)wp->len;
                dbg_last_stop_event.flags = wp->flags & BP_MEM_ACCESS;
                dbg_last_stop_event.physical = false;
            } else if (dbg_step_stop_pending) {
                dbg_last_stop_event.type = XEMU_DBG_STOP_STEP;
                dbg_last_stop_event.address = dbg_last_stop_event.pc;
                dbg_last_stop_event.length = 1;
                dbg_step_stop_pending = false;
            } else if (cpu_breakpoint_test(cs, dbg_last_stop_event.pc,
                                           BP_GDB)) {
                dbg_last_stop_event.type = XEMU_DBG_STOP_BREAKPOINT;
                dbg_last_stop_event.address = dbg_last_stop_event.pc;
                dbg_last_stop_event.length = 1;
                dbg_last_stop_event.physical = false;
            } else if (cs->singlestep_enabled || dbg_step_armed) {
                dbg_last_stop_event.type = XEMU_DBG_STOP_STEP;
                dbg_last_stop_event.address = dbg_last_stop_event.pc;
                dbg_last_stop_event.length = 1;
            } else {
                dbg_last_stop_event.type = XEMU_DBG_STOP_DEBUG;
                dbg_last_stop_event.address = dbg_last_stop_event.pc;
                dbg_last_stop_event.length = 1;
            }
        } else {
            dbg_last_stop_event.type = XEMU_DBG_STOP_DEBUG;
        }
        dbg_stop_seen_running = false;
    }

    *event = dbg_last_stop_event;
    return event->valid;
}

/* ------------------------------------------------------------------------ */
/* Address space                                                            */
/* ------------------------------------------------------------------------ */

bool xemu_dbg_to_phys(uint32_t va, uint32_t *pa)
{
    uint64_t out = 0;
    dbg_mem_access_suppress++;
    int rc = xemu_virt_to_phys(va, &out);
    dbg_mem_access_suppress--;
    if (rc != 0) {
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

    dbg_mem_access_suppress++;
    for (size_t w = 0; w < ARRAY_SIZE(windows); w++) {
        for (uint32_t p = windows[w].lo; p < windows[w].hi;
             p += TARGET_PAGE_SIZE) {
            uint64_t got = 0;
            if (xemu_virt_to_phys(p, &got) == 0 &&
                (uint32_t)(got & TARGET_PAGE_MASK) == want) {
                *va = p + (pa & ~TARGET_PAGE_MASK);
                dbg_mem_access_suppress--;
                return true;
            }
        }
    }
    dbg_mem_access_suppress--;
    return false;
}

ssize_t xemu_dbg_read_space(uint32_t addr, void *buf, size_t len, bool virt)
{
    return virt ? dbg_virt_read(addr, buf, len)
                : dbg_phys_read(addr, buf, len);
}

ssize_t xemu_dbg_write_space(uint32_t addr, const void *buf, size_t len,
                             bool virt)
{
    return virt ? dbg_virt_write(addr, buf, len)
                : dbg_phys_write(addr, buf, len);
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
    CPUState *cs;
    uint32_t eip;

    dbg_finish_stopped_step();
    if (runstate_is_running()) {
        return;
    }

    cs = qemu_get_cpu(0);
    if (cs == NULL || !dbg_prepare_guest_debug()) {
        return;
    }

    eip = dbg_get_eip(cs);
    if (eip == addr) {
        /* Already there.  Do not create a breakpoint that can only re-hit the
         * current instruction forever. */
        return;
    }

    if (!dbg_set_temp_breakpoint(cs, addr)) {
        return;
    }

    /* As with plain Resume, a breakpoint at the current EIP must be stepped
     * over once before we can run toward the temporary target. */
    if (cpu_breakpoint_test(cs, eip, BP_GDB)) {
        dbg_start_one_instruction(cs, true);
        return;
    }

    if (cs->singlestep_enabled) {
        cpu_single_step(cs, 0);
    }
    vm_start();
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
        dbg_virt_read(ebp + 4, &ret, sizeof(ret)) != (ssize_t)sizeof(ret)) {
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
