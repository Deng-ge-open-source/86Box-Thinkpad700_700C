#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(__APPLE__) && defined(__aarch64__)
#    include <pthread.h>
#endif
#include <wchar.h>
#include <math.h>
#ifndef INFINITY
#    define INFINITY (__builtin_inff())
#endif

#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x86_ops.h"
#include "x86seg_common.h"
#include "x86seg.h"
#include "x87_sf.h"
#include "x87.h"
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/nmi.h>
#include <86box/pic.h>
#include <86box/timer.h>
#include <86box/fdd.h>
#include <86box/fdc.h>
#include <86box/machine.h>
#include <86box/plat_fallthrough.h>
#include <86box/plat_unused.h>
#include <86box/gdbstub.h>
#ifdef USE_DYNAREC
#    include "codegen.h"
#    ifdef USE_NEW_DYNAREC
#        include "codegen_backend.h"
#    endif
#endif

#ifdef IS_DYNAREC
#    undef IS_DYNAREC
#endif

#include "386_common.h"

#if defined(__APPLE__) && defined(__aarch64__)
#    include <pthread.h>
#endif

#define CPU_BLOCK_END() cpu_block_end = 1

int cpu_force_interpreter   = 0;
int cpu_override_dynarec    = 0;
int inrecomp                = 0;
int cpu_block_end           = 0;
int cpu_end_block_after_ins = 0;

static uint64_t tp700_exec_trace_instructions = 0;
static int      tp700_halt_traces = 0;
static int      tp700_ac5b_entries = 0;
static unsigned tp700_int15_traces = 0;
static unsigned tp700_wait_exit_events = 0;
static unsigned tp700_wait_exit_budget = 0;
static unsigned tp700_irq2_dispatches = 0;
static unsigned tp700_dos_loop_trace = 0;
static unsigned tp700_dos_irq_dispatches = 0;
static unsigned tp700_spurious_irq_dispatches = 0;
static int      tp700_boot_iml_seen = 0;
static int      tp700_planar_iml_seen = 0;
static int      tp700_boot_sector_entry_seen = 0;
static int      tp700_dos_entry_seen = 0;
static unsigned tp700_ram_entries = 0;
static uint32_t tp700_previous_physical_pc = 0xffffffff;
static int      tp700_final_int13_active = 0;
static unsigned tp700_final_return_trace = 0;
static int      tp700_final_trace_started = 0;
static uint32_t tp700_previous_ivt1a = 0xffffffff;
static unsigned tp700_command_events = 0;

static uint32_t
tp700_find_ram_signature(const uint8_t *signature, size_t signature_size)
{
    const uint32_t ram_bytes = mem_size << 10;

    if ((ram == NULL) || (signature_size == 0) ||
        (ram_bytes < signature_size))
        return 0xffffffff;

    for (uint32_t offset = 0;
         offset <= ram_bytes - signature_size; offset++) {
        if ((ram[offset] == signature[0]) &&
            !memcmp(&ram[offset], signature, signature_size))
            return offset;
    }

    return 0xffffffff;
}

static void
tp700_check_iml_signatures(void)
{
    static const uint8_t boot_iml_signature[] = {
        0x49, 0x4d, 0x4c, 0x0e, 0x1f, 0x8f, 0x06, 0x00,
        0x02, 0x8f, 0x06, 0x02, 0x02, 0x8f, 0x06, 0xe5,
        0x01, 0x8f, 0x06, 0xe7, 0x01, 0x8f, 0x06, 0xa6,
        0x00, 0x8f, 0x06, 0xa8, 0x00, 0x8b, 0xec, 0x66
    };
    static const uint8_t planar_iml_signature[] = {
        0x35, 0x39, 0x47, 0x33, 0x36, 0x35, 0x37, 0x20,
        0x28, 0x43, 0x29, 0x20, 0x43, 0x4f, 0x50, 0x59,
        0x52, 0x49, 0x47, 0x48, 0x54, 0x20, 0x49, 0x42,
        0x4d, 0x20, 0x43, 0x4f, 0x52, 0x50, 0x4f, 0x52
    };
    uint32_t offset;

    if (!tp700_boot_iml_seen) {
        offset = tp700_find_ram_signature(boot_iml_signature,
                                          sizeof(boot_iml_signature));
        if (offset != 0xffffffff) {
            tp700_boot_iml_seen = 1;
            pclog("TP700 IML SUCCESS: $BOOT.IML loaded at physical %08X\n",
                  offset);
        }
    }

    if (!tp700_planar_iml_seen) {
        offset = tp700_find_ram_signature(planar_iml_signature,
                                          sizeof(planar_iml_signature));
        if (offset != 0xffffffff) {
            tp700_planar_iml_seen = 1;
            pclog("TP700 IML SUCCESS: $0007000.IML loaded at physical %08X\n",
                  offset);
        }
    }
}

#if defined(__aarch64__) || defined(_M_ARM64)
/* ARM64-only epoch: monotonically advances on dirty-list transitions so
   per-block retry state can distinguish dense bursts from stale retries. */
static uint32_t dynarec_s03e_dirty_epoch             = 0;
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
/* ARM64-only policy: require repeated BYTE_MASK dirty-list hits before
   NO_IMMEDIATES promotion to avoid premature slow-immediate escalation. */
/* Tuning: raise threshold to 3 consecutive dirty-list retries so transient
   churn is more likely to recover via retry-decay before forcing NO_IMMEDIATES. */
#    define DYNAREC_S03B_NO_IMM_THRESHOLD 3
/* Tuning: retry bursts must stay temporally dense; large gaps reset burst
   accumulation instead of carrying stale debt into later promotions. */
#    define DYNAREC_S03E_BURST_GAP_MAX 64
#endif

#ifdef ENABLE_386_DYNAREC_LOG
int x386_dynarec_do_log = ENABLE_386_DYNAREC_LOG;

void
x386_dynarec_log(const char *fmt, ...)
{
    va_list ap;

    if (x386_dynarec_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define x386_dynarec_log(fmt, ...)
#endif

static __inline void
fetch_ea_32_long(uint32_t rmdat)
{
    eal_r = eal_w = NULL;
    easeg         = cpu_state.ea_seg->base;
    if (cpu_rm == 4) {
        uint8_t sib = rmdat >> 8;

        switch (cpu_mod) {
            case 0:
                cpu_state.eaaddr = cpu_state.regs[sib & 7].l;
                cpu_state.pc++;
                break;
            case 1:
                cpu_state.pc++;
                cpu_state.eaaddr = ((uint32_t) (int8_t) getbyte()) + cpu_state.regs[sib & 7].l;
                break;
            case 2:
                cpu_state.eaaddr = (fastreadl(cs + cpu_state.pc + 1)) + cpu_state.regs[sib & 7].l;
                cpu_state.pc += 5;
                break;
        }
        /*SIB byte present*/
        if ((sib & 7) == 5 && !cpu_mod)
            cpu_state.eaaddr = getlong();
        else if ((sib & 6) == 4 && !cpu_state.ssegs) {
            easeg            = ss;
            cpu_state.ea_seg = &cpu_state.seg_ss;
        }
        if (((sib >> 3) & 7) != 4)
            cpu_state.eaaddr += cpu_state.regs[(sib >> 3) & 7].l << (sib >> 6);
    } else {
        cpu_state.eaaddr = cpu_state.regs[cpu_rm].l;
        if (cpu_mod) {
            if (cpu_rm == 5 && !cpu_state.ssegs) {
                easeg            = ss;
                cpu_state.ea_seg = &cpu_state.seg_ss;
            }
            if (cpu_mod == 1) {
                cpu_state.eaaddr += ((uint32_t) (int8_t) (rmdat >> 8));
                cpu_state.pc++;
            } else {
                cpu_state.eaaddr += getlong();
            }
        } else if (cpu_rm == 5) {
            cpu_state.eaaddr = getlong();
        }
    }
    if (easeg != 0xFFFFFFFF && ((easeg + cpu_state.eaaddr) & 0xFFF) <= 0xFFC) {
        uint32_t addr = easeg + cpu_state.eaaddr;
        if (readlookup2[addr >> 12] != (uintptr_t) -1)
            eal_r = (uint32_t *) (readlookup2[addr >> 12] + addr);
        if (writelookup2[addr >> 12] != (uintptr_t) -1)
            eal_w = (uint32_t *) (writelookup2[addr >> 12] + addr);
    }
}

static __inline void
fetch_ea_16_long(uint32_t rmdat)
{
    eal_r = eal_w = NULL;
    easeg         = cpu_state.ea_seg->base;
    if (!cpu_mod && cpu_rm == 6) {
        cpu_state.eaaddr = getword();
    } else {
        switch (cpu_mod) {
            case 0:
                cpu_state.eaaddr = 0;
                break;
            case 1:
                cpu_state.eaaddr = (uint16_t) (int8_t) (rmdat >> 8);
                cpu_state.pc++;
                break;
            case 2:
                cpu_state.eaaddr = getword();
                break;
        }
        cpu_state.eaaddr += (*mod1add[0][cpu_rm]) + (*mod1add[1][cpu_rm]);
        if (mod1seg[cpu_rm] == &ss && !cpu_state.ssegs) {
            easeg            = ss;
            cpu_state.ea_seg = &cpu_state.seg_ss;
        }
        cpu_state.eaaddr &= 0xFFFF;
    }
    if (easeg != 0xFFFFFFFF && ((easeg + cpu_state.eaaddr) & 0xFFF) <= 0xFFC) {
        uint32_t addr = easeg + cpu_state.eaaddr;
        if (readlookup2[addr >> 12] != (uintptr_t) -1)
            eal_r = (uint32_t *) (readlookup2[addr >> 12] + addr);
        if (writelookup2[addr >> 12] != (uintptr_t) -1)
            eal_w = (uint32_t *) (writelookup2[addr >> 12] + addr);
    }
}

#define fetch_ea_16(rmdat)       \
    cpu_state.pc++;              \
    cpu_mod = (rmdat >> 6) & 3;  \
    cpu_reg = (rmdat >> 3) & 7;  \
    cpu_rm  = rmdat & 7;         \
    if (cpu_mod != 3) {          \
        fetch_ea_16_long(rmdat); \
        if (cpu_state.abrt)      \
            return 1;            \
    }
#define fetch_ea_32(rmdat)       \
    cpu_state.pc++;              \
    cpu_mod = (rmdat >> 6) & 3;  \
    cpu_reg = (rmdat >> 3) & 7;  \
    cpu_rm  = rmdat & 7;         \
    if (cpu_mod != 3) {          \
        fetch_ea_32_long(rmdat); \
    }                            \
    if (cpu_state.abrt)          \
    return 1

#include "x86_flags.h"

#define PREFETCH_RUN(instr_cycles, bytes, modrm, reads, reads_l, writes, writes_l, ea32)      \
    do {                                                                                      \
        if (cpu_prefetch_cycles)                                                              \
            prefetch_run(instr_cycles, bytes, modrm, reads, reads_l, writes, writes_l, ea32); \
    } while (0)

#define PREFETCH_PREFIX()        \
    do {                         \
        if (cpu_prefetch_cycles) \
            prefetch_prefixes++; \
    } while (0)
#define PREFETCH_FLUSH() prefetch_flush()

#define OP_TABLE(name)   ops_##name
#if 0
#    define CLOCK_CYCLES(c)               \
        {                                 \
            if (fpu_cycles > 0) {         \
                fpu_cycles -= (c);        \
                if (fpu_cycles < 0) {     \
                    cycles += fpu_cycles; \
                }                         \
            } else {                      \
                cycles -= (c);            \
            }                             \
        }
#    define CLOCK_CYCLES_FPU(c)   cycles -= (c)
#    define CONCURRENCY_CYCLES(c) fpu_cycles = (c)
#else
#    define CLOCK_CYCLES(c)     cycles -= (c)
#    define CLOCK_CYCLES_FPU(c) cycles -= (c)
#    define CONCURRENCY_CYCLES(c)
#endif
#define CLOCK_CYCLES_ALWAYS(c) cycles -= (c)

#include "386_ops.h"

#ifdef USE_DEBUG_REGS_486
#    define CACHE_ON() (!(cr0 & (1 << 30)) && !(cpu_state.flags & T_FLAG) && !(dr[7] & 0xFF))
#else
#    define CACHE_ON() (!(cr0 & (1 << 30)) && !(cpu_state.flags & T_FLAG))
#endif

#ifdef USE_DYNAREC
int32_t         cycles_main = 0;
static int32_t  cycles_old  = 0;
static uint64_t tsc_old     = 0;

#    ifdef USE_ACYCS
int32_t acycs = 0;
#    endif

int
codegen_mmx_enter(void)
{
    MMX_ENTER();
    return 0;
}

int
codegen_femms(void)
{
    if (!cpu_has_feature(CPU_FEATURE_MMX)) {
        x86illegal();
        return 1;
    }
    if (cr0 & 0xc) {
        x86_int(7);
        return 1;
    }

    x87_emms();
    return 0;
}

int
codegen_fp_enter(void)
{
    FP_ENTER();
    return 0;
}

void
update_tsc(void)
{
    int      cycdiff;
    uint64_t delta;

    cycdiff = cycles_old - cycles;
#    ifdef USE_ACYCS
    if (inrecomp)
        cycdiff += acycs;
#    endif

    delta = tsc - tsc_old;
    if (delta > 0) {
        /* TSC has changed, this means interim timer processing has happened,
           see how much we still need to add. */
        cycdiff -= delta;
    }

    if (cycdiff > 0)
        tsc += cycdiff;

    if (cycdiff > 0) {
        if (TIMER_VAL_LESS_THAN_VAL(timer_target, (uint64_t) tsc))
            timer_process();
    }
}

static __inline void
exec386_dynarec_int(void)
{
    cpu_block_end = 0;
    x86_was_reset = 0;

    if (trap == 2) {
        /* Handle the T bit in the new TSS first. */
        CPU_BLOCK_END();
        goto block_ended;
    }

    while (!cpu_block_end) {
#    ifndef USE_NEW_DYNAREC
        oldcs  = CS;
        oldcpl = CPL;
#    endif
        cpu_state.oldpc = cpu_state.pc;
        cpu_state.op32  = use32;

        cpu_state.ea_seg = &cpu_state.seg_ds;
        cpu_state.ssegs  = 0;

        fetchdat = fastreadl_fetch(cs + cpu_state.pc);
#    ifdef ENABLE_386_DYNAREC_LOG
        if (in_smm)
            x386_dynarec_log("[%04X:%08X] fetchdat = %08X\n", CS, cpu_state.pc, fetchdat);
#    endif

        if (!cpu_state.abrt) {
            opcode = fetchdat & 0xFF;
            fetchdat >>= 8;

#    ifdef USE_DEBUG_REGS_486
            trap = (trap & ~1) | (!!(cpu_state.flags & T_FLAG));
#    else
            trap = cpu_state.flags & T_FLAG;
#    endif

            cpu_state.pc++;
#    ifdef USE_DEBUG_REGS_486
            cpu_state.eflags &= ~(RF_FLAG);
#    endif
            x86_opcodes[(opcode | cpu_state.op32) & 0x3ff](fetchdat);
        }

#    ifndef USE_NEW_DYNAREC
        if (!use32)
            cpu_state.pc &= 0xffff;
#    endif

#    ifdef USE_DEBUG_REGS_486
        if (!cpu_state.abrt) {
            if (!rf_flag_no_clear) {
                cpu_state.eflags &= ~RF_FLAG;
            }

            rf_flag_no_clear = 0;
        }
#    endif

        if (((cs + cpu_state.pc) >> 12) != pccache)
            CPU_BLOCK_END();

        if (cpu_end_block_after_ins) {
            cpu_end_block_after_ins--;
            if (!cpu_end_block_after_ins)
                CPU_BLOCK_END();
        }

        if (cpu_init)
            CPU_BLOCK_END();

        if (cpu_state.abrt)
            CPU_BLOCK_END();
        if (smi_line)
            CPU_BLOCK_END();
        else if (new_ne)
            CPU_BLOCK_END();
        else if (trap)
            CPU_BLOCK_END();
        else if (nmi && nmi_enable && nmi_mask)
            CPU_BLOCK_END();
        else if ((cpu_state.flags & I_FLAG) && pic.int_pending && !cpu_end_block_after_ins)
            CPU_BLOCK_END();
    }

block_ended:
    if (!cpu_state.abrt && !new_ne && trap) {
        if (trap & 2) dr[6] |= 0x8000;
        if (trap & 1) dr[6] |= 0x4000;
        if (trap & 16) dr[6] |= 0x2000;

        trap = 0;
#    ifndef USE_NEW_DYNAREC
        oldcs = CS;
#    endif
        cpu_state.oldpc = cpu_state.pc;
        x86_int(1);
    }

    cpu_end_block_after_ins = 0;
}

#if defined(__linux__) && !defined(__clang__) && defined(USE_NEW_DYNAREC)
static inline void __attribute__((optimize("O2")))
#else
static __inline void
#endif
exec386_dynarec_dyn(void)
{
    uint32_t start_pc  = 0;
    uint32_t phys_addr = get_phys(cs + cpu_state.pc);
    int      hash      = HASH(phys_addr);
#    ifdef USE_NEW_DYNAREC
    codeblock_t *block = &codeblock[codeblock_hash[hash]];
#    else
    codeblock_t *block = codeblock_hash[hash];
#    endif
    int valid_block = 0;

#    ifdef USE_NEW_DYNAREC
    if (!cpu_state.abrt)
#    else
    if (block && !cpu_state.abrt)
#    endif
    {
        page_t *page = &pages[phys_addr >> 12];

        /* Block must match current CS, PC, code segment size,
           and physical address. The physical address check will
           also catch any page faults at this stage */
        valid_block = (block->pc == cs + cpu_state.pc) && (block->_cs == cs) && (block->phys == phys_addr) && !((block->status ^ cpu_cur_status) & CPU_STATUS_FLAGS) && ((block->status & cpu_cur_status & CPU_STATUS_MASK) == (cpu_cur_status & CPU_STATUS_MASK));
        if (!valid_block) {
            uint64_t mask = (uint64_t) 1 << ((phys_addr >> PAGE_MASK_SHIFT) & PAGE_MASK_MASK);
#    ifdef USE_NEW_DYNAREC
            int      byte_offset = (phys_addr >> PAGE_BYTE_MASK_SHIFT) & PAGE_BYTE_MASK_OFFSET_MASK;
            uint64_t byte_mask   = 1ULL << (phys_addr & PAGE_BYTE_MASK_MASK);

            if ((page->code_present_mask & mask) ||
                ((page->mem != page_ff) && (page->byte_code_present_mask[byte_offset] & byte_mask)))
#    else
            if (page->code_present_mask[(phys_addr >> PAGE_MASK_INDEX_SHIFT) & PAGE_MASK_INDEX_MASK] & mask)
#    endif
            {
                /* Walk page tree to see if we find the correct block */
                codeblock_t *new_block = codeblock_tree_find(phys_addr, cs);
                if (new_block) {
                    valid_block = (new_block->pc == cs + cpu_state.pc) && (new_block->_cs == cs) && (new_block->phys == phys_addr) && !((new_block->status ^ cpu_cur_status) & CPU_STATUS_FLAGS) && ((new_block->status & cpu_cur_status & CPU_STATUS_MASK) == (cpu_cur_status & CPU_STATUS_MASK));
                    if (valid_block) {
                        block = new_block;
#    ifdef USE_NEW_DYNAREC
                        codeblock_hash[hash] = get_block_nr(block);
#    endif
                    }
                }
            }
        }

        if (valid_block && (block->page_mask & *block->dirty_mask)) {
#    ifdef USE_NEW_DYNAREC
            codegen_check_flush(page, page->dirty_mask, phys_addr);
            if (block->valid && (block->flags & CODEBLOCK_IN_DIRTY_LIST))
                block->flags &= ~CODEBLOCK_WAS_RECOMPILED;
            else
#    else
            codegen_check_flush(page, page->dirty_mask[(phys_addr >> 10) & 3], phys_addr);
            page->dirty_mask[(phys_addr >> 10) & 3] = 0;
#    endif
            if (!block->valid)
                valid_block = 0;
        }
        if (valid_block && block->page_mask2) {
            /* We don't want the second page to cause a page
               fault at this stage - that would break any
               code crossing a page boundary where the first
               page is present but the second isn't. Instead
               allow the first page to be interpreted and for
               the page fault to occur when the page boundary
               is actually crossed.*/
#    ifdef USE_NEW_DYNAREC
            uint32_t phys_addr_2 = get_phys_noabrt(block->pc + ((block->flags & CODEBLOCK_BYTE_MASK) ? 0x40 : 0x400));
#    else
            uint32_t phys_addr_2 = get_phys_noabrt(block->endpc);
#    endif
            page_t *page_2 = &pages[phys_addr_2 >> 12];

            if ((block->phys_2 ^ phys_addr_2) & ~0xfff)
                valid_block = 0;
            else if (block->page_mask2 & *block->dirty_mask2) {
#    ifdef USE_NEW_DYNAREC
                codegen_check_flush(page_2, page_2->dirty_mask, phys_addr_2);
                if (block->valid && (block->flags & CODEBLOCK_IN_DIRTY_LIST))
                    block->flags &= ~CODEBLOCK_WAS_RECOMPILED;
                else
#    else
                codegen_check_flush(page_2, page_2->dirty_mask[(phys_addr_2 >> 10) & 3], phys_addr_2);
                page_2->dirty_mask[(phys_addr_2 >> 10) & 3] = 0;
#    endif
                if (!block->valid)
                    valid_block = 0;
            }
        }
#    ifdef USE_NEW_DYNAREC
        /* ARM64-only: if a BYTE_MASK block executes stably outside the dirty
           list, clear stale retry debt so a distant future dirty hit does not
           trigger premature NO_IMMEDIATES promotion. */
#        if defined(__aarch64__) || defined(_M_ARM64)
        if (valid_block && !(block->flags & CODEBLOCK_IN_DIRTY_LIST) && (block->flags & CODEBLOCK_BYTE_MASK)
            && !(block->flags & CODEBLOCK_NO_IMMEDIATES) && block->dirty_list_recompile_hits) {
            block->dirty_list_recompile_hits = 0;
            block->dirty_list_last_epoch     = 0;
        }
#        endif

        if (valid_block && (block->flags & CODEBLOCK_IN_DIRTY_LIST)) {
            const int had_byte_mask     = !!(block->flags & CODEBLOCK_BYTE_MASK);
            const int had_no_immediates = !!(block->flags & CODEBLOCK_NO_IMMEDIATES);
#if defined(__aarch64__) || defined(_M_ARM64)
            const uint16_t last_epoch_before = block->dirty_list_last_epoch;
#endif
            block->flags &= ~CODEBLOCK_WAS_RECOMPILED;
            if (had_byte_mask) {
                if (!had_no_immediates) {
#if defined(__aarch64__) || defined(_M_ARM64)
                    /* ARM64-only: wait for repeated dirty-list BYTE_MASK
                       hits before NO_IMMEDIATES promotion. */
                    /* Require retries to occur in a dense burst window;
                       stale widely-spaced retries are reset. */
                    dynarec_s03e_dirty_epoch++;
                    {
                        const uint16_t cur_epoch = (uint16_t) dynarec_s03e_dirty_epoch;

                        if (last_epoch_before != 0) {
                            const uint16_t epoch_gap = (uint16_t) (cur_epoch - last_epoch_before);
                            if (epoch_gap > DYNAREC_S03E_BURST_GAP_MAX) {
                                block->dirty_list_recompile_hits = 0;
                            }
                        }
                        block->dirty_list_last_epoch = cur_epoch;
                    }
                    block->dirty_list_recompile_hits++;
                    if (block->dirty_list_recompile_hits >= DYNAREC_S03B_NO_IMM_THRESHOLD) {
                        block->flags |= CODEBLOCK_NO_IMMEDIATES;
                        block->dirty_list_last_epoch = 0;
                    }
#else
                    block->flags |= CODEBLOCK_NO_IMMEDIATES;
#endif
                }
            } else {
#if defined(__aarch64__) || defined(_M_ARM64)
                block->dirty_list_recompile_hits = 0;
                block->dirty_list_last_epoch     = 0;
#endif
                block->flags |= CODEBLOCK_BYTE_MASK;
            }
        }
        if (valid_block && (block->flags & CODEBLOCK_WAS_RECOMPILED) && (block->flags & CODEBLOCK_STATIC_TOP) && block->TOP != (cpu_state.TOP & 7))
#    else
        if (valid_block && block->was_recompiled && (block->flags & CODEBLOCK_STATIC_TOP) && block->TOP != cpu_state.TOP)
#    endif
        {
            /* FPU top-of-stack does not match the value this block was compiled
               with, re-compile using dynamic top-of-stack*/
#    ifdef USE_NEW_DYNAREC
            block->flags &= ~(CODEBLOCK_STATIC_TOP | CODEBLOCK_WAS_RECOMPILED);
#    else
            block->flags &= ~CODEBLOCK_STATIC_TOP;
            block->was_recompiled = 0;
#    endif
        }
    }

#    ifdef USE_NEW_DYNAREC
    if (valid_block && (block->flags & CODEBLOCK_WAS_RECOMPILED))
#    else
    if (valid_block && block->was_recompiled)
#    endif
    {
        void (*code)(void) = (void *) &block->data[BLOCK_START];

#    ifndef USE_NEW_DYNAREC
        codeblock_hash[hash] = block;
#    endif
        inrecomp = 1;
        code();
#    ifdef USE_ACYCS
        acycs = 0;
#    endif
        inrecomp = 0;

#    ifndef USE_NEW_DYNAREC
        if (!use32)
            cpu_state.pc &= 0xffff;
#    endif
    } else if (valid_block && !cpu_state.abrt) {
#    ifdef USE_NEW_DYNAREC
        start_pc                 = cs + cpu_state.pc;
        const int max_block_size = (block->flags & CODEBLOCK_BYTE_MASK) ? ((128 - 25) - (start_pc & 0x3f)) : 1000;
#    else
        start_pc = cpu_state.pc;
#    endif

        cpu_block_end = 0;
        x86_was_reset = 0;

#    if defined(__APPLE__) && defined(__aarch64__)
        if (__builtin_available(macOS 11.0, *)) {
            pthread_jit_write_protect_np(0);
        }
#    endif
        codegen_block_start_recompile(block);
        codegen_in_recompile = 1;

        while (!cpu_block_end) {
#    ifndef USE_NEW_DYNAREC
            oldcs  = CS;
            oldcpl = CPL;
#    endif
            cpu_state.oldpc = cpu_state.pc;
            cpu_state.op32  = use32;

            cpu_state.ea_seg = &cpu_state.seg_ds;
            cpu_state.ssegs  = 0;

            fetchdat = fastreadl_fetch(cs + cpu_state.pc);
            if ((tp700_hdd_boot_pending == 2) &&
                (((cs + cpu_state.pc) == 0x000f29dd) ||
                 ((cs + cpu_state.pc) == 0x00007c00)) &&
                !strcmp(machine_get_internal_name(), "ibmtp700")) {
                pclog("TP700 BOOT HANDOFF: committing HDD MBR after "
                      "ROM bootstrap read\n");
                tp700_boot_hdd_post();
                fetchdat = fastreadl_fetch(cs + cpu_state.pc);
            }
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                ((cs + cpu_state.pc == 0x000fd0ed) ||
                 (cs + cpu_state.pc == 0x000fd10e) ||
                 (cs + cpu_state.pc == 0x000f1669))) {
                pclog("TP700 MEMORY FLOW: phys=%08X CS:IP=%04X:%04X "
                      "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X "
                      "BP=%04X SS:SP=%04X:%04X stack=%04X %04X %04X %04X "
                      "frame=%08X %08X %08X %08X\n",
                      cs + cpu_state.pc, CS, (uint16_t) cpu_state.pc,
                      AX, BX, CX, DX, SI, DI, BP, SS, SP,
                      readmemw(ss, SP), readmemw(ss, (SP + 2) & 0xffff),
                      readmemw(ss, (SP + 4) & 0xffff),
                      readmemw(ss, (SP + 6) & 0xffff),
                      readmeml(ss, BP), readmeml(ss, (BP + 2) & 0xffff),
                      readmeml(ss, (BP + 6) & 0xffff),
                      readmeml(ss, (BP + 8) & 0xffff));
            }
#ifndef NDEBUG
            if (!strcmp(machine_get_internal_name(), "ibmtp700")) {
                const uint32_t physical_pc = cs + cpu_state.pc;

                if (!tp700_boot_sector_entry_seen &&
                    (physical_pc == 0x00007c00)) {
                    tp700_boot_sector_entry_seen = 1;
                    pclog("TP700 BOOT SUCCESS: entered disk boot sector at "
                          "%04X:%04X\n", CS, (uint16_t) cpu_state.pc);
                }
                if (!tp700_dos_entry_seen && (CS == 0x0070) &&
                    ((uint16_t) cpu_state.pc == 0x0000)) {
                    tp700_dos_entry_seen = 1;
                    pclog("TP700 DOS SUCCESS: entered IBMBIO at 0070:0000\n");
                }
                if ((physical_pc < 0x000a0000) &&
                    (tp700_previous_physical_pc >= 0x000c0000) &&
                    (tp700_ram_entries++ < 64))
                    pclog("TP700 RAM ENTRY: from physical %08X to "
                          "%04X:%04X physical %08X\n",
                          tp700_previous_physical_pc, CS,
                          (uint16_t) cpu_state.pc, physical_pc);
                tp700_previous_physical_pc = physical_pc;
            }
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                (CS == 0xf000) && (cpu_state.pc == 0x4b97)) {
                pclog("TP700 FATAL ENTRY: return=%04X SS:SP=%04X:%04X "
                      "AX=%04X BX=%04X CX=%04X DX=%04X "
                      "SI=%04X DI=%04X DS=%04X ES=%04X FLAGS=%04X\n",
                      readmemw(ss, SP), SS, SP, AX, BX, CX, DX,
                      SI, DI, DS, ES, cpu_state.flags);
            }
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                (CS == 0x0246) && (cpu_state.pc >= 0x0060) &&
                (cpu_state.pc <= 0x00a0) &&
                (tp700_dos_loop_trace++ < 384))
                pclog("TP700 DOS LOOP: CS:IP=%04X:%04X op=%08X "
                      "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X "
                      "DS=%04X ES=%04X SS:SP=%04X:%04X FLAGS=%04X "
                      "IVT1=%04X:%04X\n",
                      CS, (uint16_t) cpu_state.pc, fetchdat,
                      AX, BX, CX, DX, SI, DI, DS, ES, SS, SP,
                      cpu_state.flags, readmemw(0, 0x0006),
                      readmemw(0, 0x0004));
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                !tp700_wait_exit_budget && (tp700_wait_exit_events < 3) &&
                (CS == 0xf000) && (cpu_state.pc == 0x65ac) &&
                (ram[ds + 0x003e] & 0x80) && ((ESP & 0xffff) < 0x03c0)) {
                tp700_wait_exit_budget = 96;
                tp700_wait_exit_events++;
                pclog("TP700 WAIT EXIT BEGIN: event=%u\n", tp700_wait_exit_events);
            }
            if (tp700_wait_exit_budget) {
                pclog("TP700 WAIT EXIT: event=%u left=%u CS:IP=%04X:%04X "
                      "op=%08X SS:SP=%04X:%04X AX=%04X BX=%04X CX=%04X "
                      "DX=%04X DS=%04X FLAGS=%04X BDA3E=%02X BDA41=%02X "
                      "stack=%04X %04X %04X %04X\n",
                      tp700_wait_exit_events, tp700_wait_exit_budget,
                      CS, (uint16_t) cpu_state.pc, fetchdat,
                      SS, (uint16_t) ESP, AX, BX, CX, DX, DS,
                      cpu_state.flags, ram[ds + 0x003e], ram[ds + 0x0041],
                      readmemw(ss, SP), readmemw(ss, (SP + 2) & 0xffff),
                      readmemw(ss, (SP + 4) & 0xffff),
                      readmemw(ss, (SP + 6) & 0xffff));
                tp700_wait_exit_budget--;
            }
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                (CS == 0xf000) &&
                ((cpu_state.pc == 0x6d02) || (cpu_state.pc == 0x6d14) ||
                 (cpu_state.pc == 0x6987)) &&
                (tp700_int15_traces++ < 120)) {
                const uint32_t stack = ss + (ESP & 0xffff);

                pclog("TP700 INT15 FRAME: at=%04X:%04X SS:SP=%04X:%04X "
                      "AX=%04X FLAGS=%04X use32=%03X stack32=%d "
                      "words=%04X %04X %04X %04X %04X %04X\n",
                      CS, (uint16_t) cpu_state.pc, SS, (uint16_t) ESP,
                      AX, cpu_state.flags, use32, stack32,
                      *(uint16_t *) &ram[stack],
                      *(uint16_t *) &ram[stack + 2],
                      *(uint16_t *) &ram[stack + 4],
                      *(uint16_t *) &ram[stack + 6],
                      *(uint16_t *) &ram[stack + 8],
                      *(uint16_t *) &ram[stack + 10]);
            }
#endif
#    ifdef ENABLE_386_DYNAREC_LOG
            if (in_smm)
                x386_dynarec_log("[%04X:%08X] fetchdat = %08X\n", CS, cpu_state.pc, fetchdat);
#    endif

            if (!cpu_state.abrt) {
                opcode = fetchdat & 0xFF;
                fetchdat >>= 8;

                cpu_state.pc++;

                codegen_generate_call(opcode, x86_opcodes[(opcode | cpu_state.op32) & 0x3ff], fetchdat, cpu_state.pc, cpu_state.pc - 1);

                x86_opcodes[(opcode | cpu_state.op32) & 0x3ff](fetchdat);

                if (x86_was_reset)
                    break;
            }

#    ifndef USE_NEW_DYNAREC
            if (!use32)
                cpu_state.pc &= 0xffff;
#    endif

                /* Cap source code at 4000 bytes per block; this
                   will prevent any block from spanning more than
                   2 pages. In practice this limit will never be
                   hit, as host block size is only 2kB*/
#    ifdef USE_NEW_DYNAREC
            if (((cs + cpu_state.pc) - start_pc) >= max_block_size)
#    else
            if ((cpu_state.pc - start_pc) > 1000)
#    endif
                CPU_BLOCK_END();

            if (cpu_init)
                CPU_BLOCK_END();

            if (new_ne)
                CPU_BLOCK_END();
            if ((cpu_state.flags & T_FLAG) || (trap == 2))
                CPU_BLOCK_END();
            if (smi_line)
                CPU_BLOCK_END();
            if (nmi && nmi_enable && nmi_mask)
                CPU_BLOCK_END();
            if ((cpu_state.flags & I_FLAG) && pic.int_pending && !cpu_end_block_after_ins)
                CPU_BLOCK_END();

            if (cpu_end_block_after_ins) {
                cpu_end_block_after_ins--;
                if (!cpu_end_block_after_ins)
                    CPU_BLOCK_END();
            }

            if (cpu_state.abrt) {
                if (!(cpu_state.abrt & ABRT_EXPECTED))
                    codegen_block_remove();
                CPU_BLOCK_END();
            }
        }

        cpu_end_block_after_ins = 0;

        if ((!cpu_state.abrt || (cpu_state.abrt & ABRT_EXPECTED)) && !new_ne && !x86_was_reset)
            codegen_block_end_recompile(block);

        if (x86_was_reset)
            codegen_reset();

        codegen_in_recompile = 0;
#    if defined(__APPLE__) && defined(__aarch64__)
        if (__builtin_available(macOS 11.0, *)) {
            pthread_jit_write_protect_np(1);
        }
#    endif
    } else if (!cpu_state.abrt) {
        /* Mark block but do not recompile */
#    ifdef USE_NEW_DYNAREC
        start_pc                 = cs + cpu_state.pc;
        const int max_block_size = (block->flags & CODEBLOCK_BYTE_MASK) ? ((128 - 25) - (start_pc & 0x3f)) : 1000;
#    else
        start_pc = cpu_state.pc;
#    endif

        cpu_block_end = 0;
        x86_was_reset = 0;

        codegen_block_init(phys_addr);

        while (!cpu_block_end) {
#    ifndef USE_NEW_DYNAREC
            oldcs  = CS;
            oldcpl = CPL;
#    endif
            cpu_state.oldpc = cpu_state.pc;
            cpu_state.op32  = use32;

            cpu_state.ea_seg = &cpu_state.seg_ds;
            cpu_state.ssegs  = 0;

            codegen_endpc = (cs + cpu_state.pc) + 8;
            fetchdat      = fastreadl_fetch(cs + cpu_state.pc);

#    ifdef ENABLE_386_DYNAREC_LOG
            if (in_smm)
                x386_dynarec_log("[%04X:%08X] fetchdat = %08X\n", CS, cpu_state.pc, fetchdat);
#    endif

            if (!cpu_state.abrt) {
                opcode = fetchdat & 0xFF;
                fetchdat >>= 8;

                cpu_state.pc++;

                x86_opcodes[(opcode | cpu_state.op32) & 0x3ff](fetchdat);

                if (x86_was_reset)
                    break;
            }

#    ifndef USE_NEW_DYNAREC
            if (!use32)
                cpu_state.pc &= 0xffff;
#    endif

                /* Cap source code at 4000 bytes per block; this
                   will prevent any block from spanning more than
                   2 pages. In practice this limit will never be
                   hit, as host block size is only 2kB */
#    ifdef USE_NEW_DYNAREC
            if (((cs + cpu_state.pc) - start_pc) >= max_block_size)
#    else
            if ((cpu_state.pc - start_pc) > 1000)
#    endif
                CPU_BLOCK_END();

            if (cpu_init)
                CPU_BLOCK_END();

            if (new_ne)
                CPU_BLOCK_END();
            if (cpu_state.flags & T_FLAG)
                CPU_BLOCK_END();
            if (smi_line)
                CPU_BLOCK_END();
            if (nmi && nmi_enable && nmi_mask)
                CPU_BLOCK_END();
            if ((cpu_state.flags & I_FLAG) && pic.int_pending && !cpu_end_block_after_ins)
                CPU_BLOCK_END();

            if (cpu_end_block_after_ins) {
                cpu_end_block_after_ins--;
                if (!cpu_end_block_after_ins)
                    CPU_BLOCK_END();
            }

            if (cpu_state.abrt) {
                if (!(cpu_state.abrt & ABRT_EXPECTED))
                    codegen_block_remove();
                CPU_BLOCK_END();
            }
        }

        cpu_end_block_after_ins = 0;

        if ((!cpu_state.abrt || (cpu_state.abrt & ABRT_EXPECTED)) && !new_ne && !x86_was_reset)
            codegen_block_end();

        if (x86_was_reset)
            codegen_reset();
    }
#    ifdef USE_NEW_DYNAREC
    else
        cpu_state.oldpc = cpu_state.pc;
#    endif

}

void
exec386_dynarec(int32_t cycs)
{
    static uint64_t tp700_dynarec_slices = 0;
    static uint16_t tp700_prev_cs = 0xffff;
    static uint32_t tp700_prev_pc = 0xffffffff;
    static int      tp700_low_transitions = 0;
    static int      tp700_bad_transitions = 0;
    static int      tp700_e000_traces = 0;
    int      vector;
    int      tempi;
    int32_t  cycdiff;
    int32_t  oldcyc;
    int32_t  oldcyc2;
    uint64_t oldtsc;
    uint64_t delta;

    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
        (CS == 0) && (tp700_prev_cs != 0) && (tp700_low_transitions++ < 32))
        pclog("TP700 LOWRAM: from %04X:%08X to %04X:%08X "
              "bytes20=%02X %02X %02X %02X %02X %02X %02X %02X\n",
              tp700_prev_cs, tp700_prev_pc, CS, cpu_state.pc,
              ram[0x20], ram[0x21], ram[0x22], ram[0x23],
              ram[0x24], ram[0x25], ram[0x26], ram[0x27]);

    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
        ((tp700_prev_cs == 0xf000) || (tp700_prev_cs == 0xe000)) &&
        (CS != 0xf000) && (CS != 0xe000) && (CS != 0x0040) &&
        (tp700_bad_transitions++ < 32)) {
        const uint32_t stack = ss + (ESP & 0xffff);

        pclog("TP700 TRANSITION: from %04X:%08X to %04X:%08X "
              "SS:SP=%04X:%04X stack=%08X top=%02X %02X %02X %02X\n",
              tp700_prev_cs, tp700_prev_pc, CS, cpu_state.pc,
              SS, ESP & 0xffff, stack,
              ram[stack], ram[stack + 1], ram[stack + 2], ram[stack + 3]);
    }

    tp700_prev_cs = CS;
    tp700_prev_pc = cpu_state.pc;

    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
        (CS == 0xe000) && (cpu_state.pc >= 0x7000) &&
        (tp700_e000_traces++ < 1024)) {
        const uint32_t stack = ss + (ESP & 0xffff);
        pclog("TP700 E000: %04X:%08X SS:SP=%04X:%04X "
              "stack=%08X top=%02X %02X %02X %02X\n",
              CS, cpu_state.pc, SS, ESP & 0xffff, stack,
              ram[stack], ram[stack + 1], ram[stack + 2], ram[stack + 3]);
    }

    /* Keep the slice counter for the one-shot IML detector, but do not write
       a diagnostic line every 256 slices.  That trace was useful while the
       ROM hand-off was being implemented, but it causes visible mouse and
       menu stutter in Windows 3.x by synchronously growing the log forever. */
    if (!strcmp(machine_get_internal_name(), "ibmtp700"))
        ++tp700_dynarec_slices;

    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
        ((tp700_dynarec_slices & 0xff) == 0) &&
        (!tp700_boot_iml_seen || !tp700_planar_iml_seen))
        tp700_check_iml_signatures();

    int32_t cyc_period = cycs / (force_10ms ? 2000 : 200); /*5us*/

#    ifdef USE_ACYCS
    acycs = 0;
#    endif
    cycles_main += cycs;
    while (cycles_main > 0) {
        int32_t cycles_start;

        cycles += cyc_period;
        cycles_start = cycles;

        while (cycles > 0) {
#    ifndef USE_NEW_DYNAREC
            oldcs           = CS;
            cpu_state.oldpc = cpu_state.pc;
            oldcpl          = CPL;
            cpu_state.op32  = use32;

            cycdiff = 0;
#    endif
            oldcyc = oldcyc2 = cycles;
            cycles_old       = cycles;
            oldtsc           = tsc;
            tsc_old          = tsc;
            if (cpu_force_interpreter || cpu_override_dynarec ||  (!CACHE_ON())) /*Interpret block*/
            {
                exec386_dynarec_int();
            } else {
                exec386_dynarec_dyn();
            }

            if (cpu_init) {
                cpu_init = 0;
                resetx86();
            }

            if (cpu_state.abrt) {
                flags_rebuild();
                tempi          = cpu_state.abrt & ABRT_MASK;
                cpu_state.abrt = 0;
                x86_doabrt(tempi);
                if (cpu_state.abrt) {
                    cpu_state.abrt = 0;
                    cpu_state.pc   = cpu_state.oldpc;
#    ifndef USE_NEW_DYNAREC
                    CS = oldcs;
#    endif
                    pmodeint(8, 0);
                    if (cpu_state.abrt) {
                        cpu_state.abrt = 0;
                        softresetx86();
                        cpu_set_edx();
#    ifdef ENABLE_386_DYNAREC_LOG
                        x386_dynarec_log("Triple fault - reset\n");
#    endif
                    }
                }
            }

            if (new_ne) {
#    ifndef USE_NEW_DYNAREC
                oldcs = CS;
#    endif
                cpu_state.oldpc = cpu_state.pc;
                new_ne = 0;
                x86_int(16);
            }

            if (smi_line)
                enter_smm_check(0);
            else if (nmi && nmi_enable && nmi_mask) {
#    ifndef USE_NEW_DYNAREC
                oldcs = CS;
#    endif
                cpu_state.oldpc = cpu_state.pc;
                x86_int(2);
                nmi_enable = 0;
#    ifdef OLD_NMI_BEHAVIOR
                if (nmi_auto_clear) {
                    nmi_auto_clear = 0;
                    nmi            = 0;
                }
#    else
                nmi = 0;
#    endif
            } else if ((cpu_state.flags & I_FLAG) && pic.int_pending) {
                vector = picinterrupt();
                if (vector != -1) {
                    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                        (vector == 0x0f) &&
                        (tp700_spurious_irq_dispatches++ < 128))
                        pclog("TP700 IRQ7/SPURIOUS: from=%04X:%04X "
                              "master=%02X/%02X/%02X pending=%u int=%02X "
                              "slave=%02X/%02X/%02X pending=%u\n",
                              CS, (uint16_t) cpu_state.pc,
                              pic.irr, pic.isr, pic.imr,
                              pic.int_pending, pic.interrupt,
                              pic2.irr, pic2.isr, pic2.imr,
                              pic2.int_pending);
                    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                        (vector == 0x0a)) {
                        const unsigned trace_index = tp700_irq2_dispatches++;

                        if ((trace_index < 64) ||
                            ((trace_index & 0xffff) == 0))
                            pclog("TP700 IRQ2 DISPATCH: index=%u from=%04X:%04X "
                                  "master=%02X/%02X/%02X "
                                  "slave=%02X/%02X/%02X pending=%u/%u\n",
                                  trace_index, CS, (uint16_t) cpu_state.pc,
                                  pic.irr, pic.isr, pic.imr,
                                  pic2.irr, pic2.isr, pic2.imr,
                                  pic.int_pending, pic2.int_pending);
                    }
#    ifndef USE_NEW_DYNAREC
                    oldcs = CS;
#    endif
                    cpu_state.oldpc = cpu_state.pc;
                    x86_int(vector);
                }
            }

            cycdiff = oldcyc - cycles;
            delta   = tsc - oldtsc;
            if (delta > 0) {
                /* TSC has changed, this means interim timer processing has happened,
                   see how much we still need to add. */
                cycdiff -= delta;
                if (cycdiff > 0)
                    tsc += cycdiff;
            } else {
                /* TSC has not changed. */
                tsc += cycdiff;
            }

            if (cycdiff > 0) {
                if (TIMER_VAL_LESS_THAN_VAL(timer_target, (uint64_t) tsc))
                    timer_process();
            }

#    ifdef USE_GDBSTUB
            if (gdbstub_instruction())
                return;
#    endif
        }

        cycles_main -= (cycles_start - cycles);
    }
}
#endif

void
exec386(int32_t cycs)
{
    int      vector;
    int      tempi;
    int32_t  cycdiff;
    int32_t  oldcyc;
    int32_t  cycle_period;
    int32_t  ins_cycles;
    uint32_t addr;

    cycles += cycs;

    while (cycles > 0) {
        cycle_period = (timer_target - (uint64_t) tsc) + 1;

        x86_was_reset = 0;
        cycdiff       = 0;
        oldcyc        = cycles;
        while (cycdiff < cycle_period) {
#ifdef USE_DEBUG_REGS_486
            int ins_fetch_fault = 0;
#endif
            ins_cycles = cycles;

#ifndef USE_NEW_DYNAREC
            oldcs  = CS;
            oldcpl = CPL;
#endif
            cpu_state.oldpc = cpu_state.pc;
            cpu_state.op32  = use32;

#ifndef USE_NEW_DYNAREC
            x86_was_reset = 0;
#endif

            cpu_state.ea_seg = &cpu_state.seg_ds;
            cpu_state.ssegs  = 0;

#ifdef USE_DEBUG_REGS_486
            if (is386)
                ins_fetch_fault = cpu_386_check_instruction_fault();

            /* Breakpoint fault has priority over other faults. */
            if ((cpu_state.abrt == 0) & ins_fetch_fault) {
                x86gen();
                ins_fetch_fault = 0;
                /* No instructions executed at this point. */
                goto block_ended;
            }
#endif

            fetchdat = fastreadl_fetch(cs + cpu_state.pc);
            if ((tp700_hdd_boot_pending == 2) &&
                (((cs + cpu_state.pc) == 0x000f29dd) ||
                 ((cs + cpu_state.pc) == 0x00007c00)) &&
                !strcmp(machine_get_internal_name(), "ibmtp700")) {
                pclog("TP700 BOOT HANDOFF: committing HDD MBR after "
                      "ROM bootstrap read\n");
                tp700_boot_hdd_post();
                fetchdat = fastreadl_fetch(cs + cpu_state.pc);
            }
#ifndef NDEBUG
            if (!strcmp(machine_get_internal_name(), "ibmtp700")) {
                const uint32_t physical_pc = cs + cpu_state.pc;

                if (!tp700_boot_sector_entry_seen &&
                    (physical_pc == 0x00007c00)) {
                    tp700_boot_sector_entry_seen = 1;
                    pclog("TP700 BOOT SUCCESS: entered disk boot sector at "
                          "%04X:%04X\n", CS, (uint16_t) cpu_state.pc);
                }
                if (!tp700_dos_entry_seen && (CS == 0x0070) &&
                    ((uint16_t) cpu_state.pc == 0x0000)) {
                    tp700_dos_entry_seen = 1;
                    pclog("TP700 DOS SUCCESS: entered IBMBIO at 0070:0000\n");
                }
                if ((physical_pc < 0x000a0000) &&
                    (tp700_previous_physical_pc >= 0x000c0000) &&
                    (tp700_ram_entries++ < 64))
                    pclog("TP700 RAM ENTRY: from physical %08X to "
                          "%04X:%04X physical %08X\n",
                          tp700_previous_physical_pc, CS,
                          (uint16_t) cpu_state.pc, physical_pc);
                tp700_previous_physical_pc = physical_pc;
            }
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                (CS == 0x0246) && (cpu_state.pc >= 0x0060) &&
                (cpu_state.pc <= 0x00a0) &&
                (tp700_dos_loop_trace++ < 384))
                pclog("TP700 DOS LOOP: CS:IP=%04X:%04X op=%08X "
                      "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X "
                      "DS=%04X ES=%04X SS:SP=%04X:%04X FLAGS=%04X "
                      "IVT1=%04X:%04X\n",
                      CS, (uint16_t) cpu_state.pc, fetchdat,
                      AX, BX, CX, DX, SI, DI, DS, ES, SS, SP,
                      cpu_state.flags, readmemw(0, 0x0006),
                      readmemw(0, 0x0004));
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                !tp700_wait_exit_budget && (tp700_wait_exit_events < 3) &&
                (CS == 0xf000) && (cpu_state.pc == 0x65ac) &&
                (readmemb(ds, 0x003e) & 0x80) &&
                ((ESP & 0xffff) < 0x03c0)) {
                tp700_wait_exit_budget = 96;
                tp700_wait_exit_events++;
                pclog("TP700 WAIT EXIT REAL BEGIN: event=%u\n",
                      tp700_wait_exit_events);
            }
            if (tp700_wait_exit_budget) {
                pclog("TP700 WAIT EXIT REAL: event=%u left=%u CS:IP=%04X:%04X "
                      "op=%08X SS:SP=%04X:%04X AX=%04X BX=%04X CX=%04X "
                      "DX=%04X DS=%04X FLAGS=%04X DS3E=%02X DS41=%02X "
                      "stack=%04X %04X %04X %04X\n",
                      tp700_wait_exit_events, tp700_wait_exit_budget,
                      CS, (uint16_t) cpu_state.pc, fetchdat,
                      SS, (uint16_t) ESP, AX, BX, CX, DX, DS,
                      cpu_state.flags, readmemb(ds, 0x003e),
                      readmemb(ds, 0x0041),
                      readmemw(ss, SP), readmemw(ss, (SP + 2) & 0xffff),
                      readmemw(ss, (SP + 4) & 0xffff),
                      readmemw(ss, (SP + 6) & 0xffff));
                tp700_wait_exit_budget--;
            }
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                (CS == 0xf000) && (cpu_state.pc == 0xac5b) &&
                (tp700_ac5b_entries++ < 8)) {
                const uint32_t stack = ss + (ESP & 0xffff);

                pclog("TP700 AC5B ENTRY: SS:SP=%04X:%04X stack=%08X "
                      "top=%02X %02X %02X %02X %02X %02X "
                      "nmi=%d enable=%d mask=%02X state=%02X\n",
                      SS, ESP & 0xffff, stack,
                      ram[stack], ram[stack + 1], ram[stack + 2],
                      ram[stack + 3], ram[stack + 4], ram[stack + 5],
                      nmi, nmi_enable, nmi_mask,
                      ram[readmemw(0, 0x040e) * 16 + 1]);
            }
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                (CS == 0xf000) && (cpu_state.pc == 0xaf5f) &&
                (tp700_halt_traces++ < 4)) {
                const uint32_t state_addr = ds + (ESI & 0xffff) + 1;
                const uint32_t stack      = ss + (ESP & 0xffff);

                pclog("TP700 HALT: DS:SI=%04X:%04X state=%08X/%02X "
                      "SS:SP=%04X:%04X stack=%08X top=%02X %02X %02X %02X "
                      "IVT2=%02X%02X:%02X%02X BDA0E=%02X%02X\n",
                      DS, ESI & 0xffff, state_addr, ram[state_addr],
                      SS, ESP & 0xffff, stack,
                      ram[stack], ram[stack + 1], ram[stack + 2], ram[stack + 3],
                      ram[11], ram[10], ram[9], ram[8], ram[0x40f], ram[0x40e]);
            }
            ++tp700_exec_trace_instructions;
            if (!strcmp(machine_get_internal_name(), "ibmtp700")) {
                const uint32_t ivt1a = *(uint32_t *) &ram[0x0068];

                if (ivt1a != tp700_previous_ivt1a) {
                    pclog("TP700 IVT1A CHANGE: at=%04X:%04X old=%08X "
                          "new=%04X:%04X\n",
                          CS, (uint16_t) cpu_state.pc, tp700_previous_ivt1a,
                          (uint16_t) (ivt1a >> 16), (uint16_t) ivt1a);
                    tp700_previous_ivt1a = ivt1a;
                }
                if ((tp700_final_int13_active) && (CS >= 0x1000) &&
                    (((uint16_t) cpu_state.pc == 0x4429) ||
                     ((uint16_t) cpu_state.pc == 0x4434) ||
                     ((uint16_t) cpu_state.pc == 0x443e) ||
                     ((uint16_t) cpu_state.pc == 0x4441) ||
                     ((uint16_t) cpu_state.pc == 0x4443) ||
                     ((uint16_t) cpu_state.pc == 0x4446) ||
                     ((uint16_t) cpu_state.pc == 0x444f) ||
                     ((uint16_t) cpu_state.pc == 0x4454) ||
                     ((uint16_t) cpu_state.pc == 0x5244) ||
                     ((uint16_t) cpu_state.pc == 0x5277) ||
                     ((uint16_t) cpu_state.pc == 0x5280) ||
                     ((uint16_t) cpu_state.pc == 0x528a) ||
                     ((uint16_t) cpu_state.pc == 0x528c) ||
                     ((uint16_t) cpu_state.pc == 0x5290) ||
                     ((uint16_t) cpu_state.pc == 0x52af) ||
                     ((uint16_t) cpu_state.pc == 0x52c5) ||
                     ((uint16_t) cpu_state.pc == 0x52c6) ||
                     ((uint16_t) cpu_state.pc == 0x4ba7)) &&
                    (tp700_command_events++ < 64)) {
                    pclog("TP700 COMMAND CHECK: CS:IP=%04X:%04X AX=%04X "
                          "BX=%04X CX=%04X DX=%04X BP=%04X FLAGS=%04X "
                          "TAG=%02X%02X%02X%02X%02X%02X%02X%02X\n",
                          CS, (uint16_t) cpu_state.pc, AX, BX, CX, DX, BP,
                          cpu_state.flags,
                          readmemb(ds, 0x430e), readmemb(ds, 0x430f),
                          readmemb(ds, 0x4310), readmemb(ds, 0x4311),
                          readmemb(ds, 0x4312), readmemb(ds, 0x4313),
                          readmemb(ds, 0x4314), readmemb(ds, 0x4315));
                }
            }
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                (CS == 0x9f44) && (cpu_state.pc == 0x0420) &&
                (AX == 0x0201) && (BX == 0x7c5a) && (CX == 0x0209))
                tp700_final_int13_active = 1;
            if (tp700_final_int13_active && !tp700_final_trace_started &&
                (CS == 0x0070) && (cpu_state.pc == 0x18c3)) {
                tp700_final_trace_started = 1;
                tp700_final_return_trace = 2500;
                pclog("TP700 INT15 C000 TRACE START: CS:IP=%04X:%04X "
                      "AX=%04X BX=%04X ES=%04X SS:SP=%04X:%04X\n",
                      CS, (uint16_t) cpu_state.pc, AX, BX, ES, SS, SP);
            }
            if (tp700_final_return_trace) {
                pclog("TP700 FINAL RETURN TRACE: left=%u CS:IP=%04X:%04X "
                      "op=%08X SS:SP=%04X:%04X top=%04X %04X %04X %04X "
                      "AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X "
                      "FLAGS=%04X\n",
                      tp700_final_return_trace--, CS,
                      (uint16_t) cpu_state.pc, fetchdat, SS, SP,
                      readmemw(ss, SP), readmemw(ss, (SP + 2) & 0xffff),
                      readmemw(ss, (SP + 4) & 0xffff),
                      readmemw(ss, (SP + 6) & 0xffff),
                      AX, BX, CX, DX, DS, ES, cpu_state.flags);
            }
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                ((tp700_exec_trace_instructions & 0x0000ffff) == 0) &&
                (!tp700_boot_iml_seen || !tp700_planar_iml_seen))
                tp700_check_iml_signatures();
            if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                ((tp700_exec_trace_instructions <= 2048) ||
                 ((tp700_exec_trace_instructions & 0x0000ffff) == 0))) {
                pclog("TP700 EXEC: ins=%llu CS:IP=%04X:%08X phys=%08X op=%08X "
                      "AX=%08X BX=%08X CX=%08X DX=%08X SP=%08X\n",
                      (unsigned long long) tp700_exec_trace_instructions,
                      CS, cpu_state.pc, cs + cpu_state.pc, fetchdat,
                      EAX, EBX, ECX, EDX, ESP);
            }
#endif

            if (!cpu_state.abrt) {
#ifdef ENABLE_386_LOG
                if (in_smm)
                    x386_dynarec_log("[%04X:%08X] %08X\n", CS, cpu_state.pc, fetchdat);
#endif
                opcode = fetchdat & 0xFF;
                fetchdat >>= 8;
#ifdef USE_DEBUG_REGS_486
                trap = (trap & ~1) | (!!(cpu_state.flags & T_FLAG));
#else
                trap = cpu_state.flags & T_FLAG;
#endif

                cpu_state.pc++;
#ifdef USE_DEBUG_REGS_486
                cpu_state.eflags &= ~(RF_FLAG);
#endif
                x86_opcodes[(opcode | cpu_state.op32) & 0x3ff](fetchdat);
                if (x86_was_reset)
                    break;
            }
#ifdef ENABLE_386_LOG
            else if (in_smm)
                x386_dynarec_log("[%04X:%08X] ABRT\n", CS, cpu_state.pc);
#endif

            if (cpu_flush_pending == 1)
                cpu_flush_pending++;
            else if (cpu_flush_pending == 2) {
                cpu_flush_pending = 0;
                flushmmucache_pc();
            }

#ifndef USE_NEW_DYNAREC
            if (!use32)
                cpu_state.pc &= 0xffff;
#endif

            if (cpu_end_block_after_ins)
                cpu_end_block_after_ins--;

#ifdef USE_DEBUG_REGS_486
block_ended:
#endif
            if (cpu_state.abrt) {
                uint8_t oop    = opcode;
                flags_rebuild();
                tempi          = cpu_state.abrt & ABRT_MASK;
                cpu_state.abrt = 0;
                x86_doabrt(tempi);
                if (cpu_state.abrt) {
                    pclog("Double fault - %02X\n", oop);
                    cpu_state.abrt = 0;
#ifndef USE_NEW_DYNAREC
                    CS = oldcs;
#endif
                    cpu_state.pc = cpu_state.oldpc;
                    x386_dynarec_log("Double fault\n");
                    pmodeint(8, 0);
                    if (cpu_state.abrt) {
                        cpu_state.abrt = 0;
                        softresetx86();
                        cpu_set_edx();
#ifdef ENABLE_386_LOG
                        x386_dynarec_log("Triple fault - reset\n");
#endif
                    }
                }

#ifdef USE_DEBUG_REGS_486
                if (is386 && !x86_was_reset  && ins_fetch_fault)
                    x86gen();
#endif
            } else if (new_ne) {
                flags_rebuild();

                new_ne = 0;
#ifndef USE_NEW_DYNAREC
                oldcs = CS;
#endif
                cpu_state.oldpc = cpu_state.pc;
                x86_int(16);
            } else if (trap) {
                flags_rebuild();
#ifdef USE_DEBUG_REGS_486
                if (trap & 2) dr[6] |= 0x8000;
                if (trap & 1) dr[6] |= 0x4000;
                if (trap & 16) dr[6] |= 0x2000;
#endif
                trap = 0;
#ifndef USE_NEW_DYNAREC
                oldcs = CS;
#endif
                cpu_state.oldpc = cpu_state.pc;
                x86_int(1);
            }

            if (smi_line)
                enter_smm_check(0);
            else if (nmi && nmi_enable && nmi_mask) {
#ifndef USE_NEW_DYNAREC
                oldcs = CS;
#endif
                cpu_state.oldpc = cpu_state.pc;
                x86_int(2);
                nmi_enable = 0;
#ifdef OLD_NMI_BEHAVIOR
                if (nmi_auto_clear) {
                    nmi_auto_clear = 0;
                    nmi            = 0;
                }
#else
                nmi = 0;
#endif
            } else if ((cpu_state.flags & I_FLAG) && pic.int_pending && !cpu_end_block_after_ins) {
                vector = picinterrupt();
                if (vector != -1) {
                    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                        (CS == 0x0246) && (cpu_state.pc >= 0x0060) &&
                        (cpu_state.pc <= 0x00a0) &&
                        (tp700_dos_irq_dispatches++ < 128))
                        pclog("TP700 DOS IRQ: vector=%02X from=%04X:%04X "
                              "target=%04X:%04X "
                              "ivt1c=%04X:%04X "
                              "FLAGS=%04X master=%02X/%02X/%02X "
                              "slave=%02X/%02X/%02X\n",
                              vector, CS, (uint16_t) cpu_state.pc,
                              readmemw(0, (vector << 2) + idt.base + 2),
                              readmemw(0, (vector << 2) + idt.base),
                              readmemw(0, 0x0072), readmemw(0, 0x0070),
                              cpu_state.flags, pic.irr, pic.isr, pic.imr,
                              pic2.irr, pic2.isr, pic2.imr);
                    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                        (vector == 0x0a)) {
                        const unsigned trace_index = tp700_irq2_dispatches++;

                        if ((trace_index < 64) ||
                            ((trace_index & 0xffff) == 0))
                            pclog("TP700 IRQ2 DISPATCH: index=%u from=%04X:%04X "
                                  "master=%02X/%02X/%02X "
                                  "slave=%02X/%02X/%02X pending=%u/%u\n",
                                  trace_index, CS, (uint16_t) cpu_state.pc,
                                  pic.irr, pic.isr, pic.imr,
                                  pic2.irr, pic2.isr, pic2.imr,
                                  pic.int_pending, pic2.int_pending);
                    }
                    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
                        (vector >= 0x70) && (vector <= 0x77)) {
                        const uint32_t ivt_addr = (vector << 2) + idt.base;

                        pclog("TP700 SLAVE IRQ: vector=%02X from=%04X:%04X "
                              "target=%04X:%04X SS:SP=%04X:%04X\n",
                              vector, CS, cpu_state.pc,
                              readmemw(0, ivt_addr + 2), readmemw(0, ivt_addr),
                              SS, SP);
                    }
                    flags_rebuild();
                    if (msw & 1)
                        pmodeint(vector, 0);
                    else {
                        writememw(ss, (SP - 2) & 0xFFFF, cpu_state.flags);
                        writememw(ss, (SP - 4) & 0xFFFF, CS);
                        writememw(ss, (SP - 6) & 0xFFFF, cpu_state.pc);
                        SP -= 6;
                        addr = (vector << 2) + idt.base;
                        cpu_state.flags &= ~I_FLAG;
                        cpu_state.flags &= ~T_FLAG;
                        cpu_state.pc = readmemw(0, addr);
                        loadcs(readmemw(0, addr + 2));
                    }
                }
            }

            ins_cycles -= cycles;
            tsc += ins_cycles;

            cycdiff = oldcyc - cycles;

            if (timetolive) {
                timetolive--;
                if (!timetolive)
                    fatal("Life expired\n");
            }

            if (TIMER_VAL_LESS_THAN_VAL(timer_target, (uint64_t) tsc))
                timer_process();

#ifdef USE_GDBSTUB
            if (gdbstub_instruction())
                return;
#endif
        }
    }
}
