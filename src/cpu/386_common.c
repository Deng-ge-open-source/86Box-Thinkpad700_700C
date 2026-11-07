#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#ifndef INFINITY
#    define INFINITY (__builtin_inff())
#endif
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/timer.h>
#include "x86.h"
#include "x86seg_common.h"
#include "x87_sf.h"
#include "x87.h"
#include <86box/nmi.h>
#include <86box/mem.h>
#include <86box/smram.h>
#include <86box/pic.h>
#include <86box/pit.h>
#include <86box/fdd.h>
#include <86box/fdc.h>
#include <86box/keyboard.h>
#include <86box/io.h>
#include <86box/machine.h>
#include <86box/hdd.h>
#include <86box/timer.h>

#include "x86seg.h"
#include "386_common.h"
#include "x86_flags.h"
#include <86box/plat_unused.h>

#ifdef USE_DYNAREC
#    include "codegen.h"
#    define CPU_BLOCK_END() cpu_block_end = 1
#else
#    define CPU_BLOCK_END()
#endif

x86seg gdt;
x86seg ldt;
x86seg idt;
x86seg tr;

uint32_t cr2;
uint32_t cr3;
uint32_t cr4;
uint32_t dr[8];

uint32_t use32;
int      stack32;

int      cpu_init = 0;

uint32_t *eal_r;
uint32_t *eal_w;

int nmi_enable = 1;

int alt_access;
int cpl_override = 0;

#ifdef USE_NEW_DYNAREC
uint16_t cpu_cur_status = 0;
#else
uint32_t cpu_cur_status = 0;
#endif

extern uint8_t *pccache2;

extern int      optype;
extern uint32_t pccache;

int      new_ne            = 0;
int      in_sys            = 0;
int      unmask_a20_in_smm = 0;
uint32_t old_rammask       = 0xffffffff;

int soft_reset_mask = 0;

int smi_latched = 0;
int smm_in_hlt  = 0;
int smi_block   = 0;

int prefetch_prefixes = 0;
int rf_flag_no_clear = 0;

int tempc;
int oldcpl;
int optype;
int inttype;
int oddeven = 0;
int timetolive;

uint16_t oldcs;

uint32_t oldds;
uint32_t oldss;
uint32_t olddslimit;
uint32_t oldsslimit;
uint32_t olddslimitw;
uint32_t oldsslimitw;
uint32_t oxpc;
uint32_t rmdat32;
uint32_t backupregs[16];

x86seg _oldds;

uint8_t rep_op = 0x00;
uint8_t is_smint = 0;

uint16_t io_port = 0x0000;
uint32_t io_val  = 0x00000000;

int opcode_has_modrm[256] = {
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0, /*00*/
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0, /*10*/
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0, /*20*/
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0, /*30*/

    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*40*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*50*/
    0, 0, 1, 1,  0, 0, 0, 0,  0, 1, 0, 1,  0, 0, 0, 0, /*60*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*70*/

    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1, /*80*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*90*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*a0*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*b0*/

    1, 1, 0, 0,  1, 1, 1, 1,  0, 0, 0, 0,  0, 0, 0, 0, /*c0*/
    1, 1, 1, 1,  0, 0, 0, 0,  1, 1, 1, 1,  1, 1, 1, 1, /*d0*/
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0, /*e0*/
    0, 0, 0, 0,  0, 0, 1, 1,  0, 0, 0, 0,  0, 0, 1, 1, /*f0*/
};

int opcode_length[256] = { 3, 3, 3, 3, 3, 3, 1, 1, 3, 3, 3, 3, 3, 3, 1, 3,   /* 0x0x */
                           3, 3, 3, 3, 3, 3, 1, 1, 3, 3, 3, 3, 3, 3, 1, 1,   /* 0x1x */
                           3, 3, 3, 3, 3, 3, 1, 1, 3, 3, 3, 3, 3, 3, 1, 1,   /* 0x2x */
                           3, 3, 3, 3, 3, 3, 1, 1, 3, 3, 3, 3, 3, 3, 1, 1,   /* 0x3x */
                           1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,   /* 0x4x */
                           1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,   /* 0x5x */
                           1, 1, 3, 3, 1, 1, 1, 1, 3, 3, 2, 3, 1, 1, 1, 1,   /* 0x6x */
                           2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,   /* 0x7x */
                           3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,   /* 0x8x */
                           1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1,   /* 0x9x */
                           3, 3, 3, 3, 1, 1, 1, 1, 2, 3, 1, 1, 1, 1, 1, 1,   /* 0xax */
                           2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,   /* 0xbx */
                           3, 3, 3, 1, 3, 3, 3, 3, 3, 1, 3, 1, 1, 2, 1, 1,   /* 0xcx */
                           3, 3, 3, 3, 2, 2, 1, 1, 3, 3, 3, 3, 3, 3, 3, 3,   /* 0xdx */
                           2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 2, 1, 1, 1, 1,   /* 0xex */
                           1, 1, 1, 1, 1, 1, 3, 3, 1, 1, 1, 1, 1, 1, 3, 3 }; /* 0xfx */

/* 0 = no, 1 = always, 2 = depends on second opcode, 3 = depends on mod/rm */
int lock_legal[256]    = { 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 2,   /* 0x0x */
                           1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0,   /* 0x1x */
                           1, 1, 1, 1, 1, 1, 4, 0, 1, 1, 1, 1, 1, 1, 4, 0,   /* 0x2x */
                           1, 1, 1, 1, 1, 1, 4, 0, 0, 0, 0, 0, 0, 0, 4, 0,   /* 0x3x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x4x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x5x */
                           0, 0, 0, 0, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x6x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x7x */
                           3, 3, 3, 3, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x8x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x9x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xax */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xbx */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xcx */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xdx */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xex */
                           0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 0, 0, 0, 0, 3, 3 }; /* 0xfx */

int lock_legal_0f[256] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x0x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x1x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x2x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x3x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x4x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x5x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x6x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x7x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x8x */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0x9x */
                           0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,   /* 0xax */
                           0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 3, 1, 0, 0, 0, 0,   /* 0xbx */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xcx */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xdx */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* 0xex */
                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }; /* 0xfx */

/* (modrm >> 3) & 0x07 */
int lock_legal_ba[8]   = { 0, 0, 0, 0, 1, 1, 1, 1 };

/* Also applies to 81, 82, and 83 */
int lock_legal_80[8]   = { 1, 1, 1, 1, 1, 1, 1, 0 };

/* Also applies to F7 */
int lock_legal_f6[8]   = { 0, 0, 1, 1, 0, 0, 0, 0 };

/* Also applies to FF */
int lock_legal_fe[8]   = { 1, 1, 0, 0, 0, 0, 0, 0 };

uint32_t addr64;
uint32_t addr64_2;
uint32_t addr64a[8];
uint32_t addr64a_2[8];

static pc_timer_t *cpu_fast_off_timer  = NULL;
static double      cpu_fast_off_period = 0.0;

#define AMD_SYSCALL_EIP (msr.amd_star & 0xFFFFFFFF)
#define AMD_SYSCALL_SB  ((msr.amd_star >> 32) & 0xFFFF)
#define AMD_SYSRET_SB   ((msr.amd_star >> 48) & 0xFFFF)

/* These #define's and enum have been borrowed from Bochs. */
/* SMM feature masks */
#define SMM_IO_INSTRUCTION_RESTART (0x00010000)
#define SMM_SMBASE_RELOCATION      (0x00020000)
#define SMM_REVISION               (0x20000000)

/* TODO: Which CPU added SMBASE relocation? */
#define SMM_REVISION_ID         (SMM_SMBASE_RELOCATION | SMM_IO_INSTRUCTION_RESTART | SMM_REVISION)

#define SMM_SAVE_STATE_MAP_SIZE 128

enum SMMRAM_Fields_386_To_P5 {
    SMRAM_FIELD_P5_CR0 = 0,          /* 1FC */
    SMRAM_FIELD_P5_CR3,              /* 1F8 */
    SMRAM_FIELD_P5_EFLAGS,           /* 1F4 */
    SMRAM_FIELD_P5_EIP,              /* 1F0 */
    SMRAM_FIELD_P5_EDI,              /* 1EC */
    SMRAM_FIELD_P5_ESI,              /* 1E8 */
    SMRAM_FIELD_P5_EBP,              /* 1E4 */
    SMRAM_FIELD_P5_ESP,              /* 1E0 */
    SMRAM_FIELD_P5_EBX,              /* 1DC */
    SMRAM_FIELD_P5_EDX,              /* 1D8 */
    SMRAM_FIELD_P5_ECX,              /* 1D4 */
    SMRAM_FIELD_P5_EAX,              /* 1D0 */
    SMRAM_FIELD_P5_DR6,              /* 1CC */
    SMRAM_FIELD_P5_DR7,              /* 1C8 */
    SMRAM_FIELD_P5_TR_SELECTOR,      /* 1C4 */
    SMRAM_FIELD_P5_LDTR_SELECTOR,    /* 1C0 */
    SMRAM_FIELD_P5_GS_SELECTOR,      /* 1BC */
    SMRAM_FIELD_P5_FS_SELECTOR,      /* 1B8 */
    SMRAM_FIELD_P5_DS_SELECTOR,      /* 1B4 */
    SMRAM_FIELD_P5_SS_SELECTOR,      /* 1B0 */
    SMRAM_FIELD_P5_CS_SELECTOR,      /* 1AC */
    SMRAM_FIELD_P5_ES_SELECTOR,      /* 1A8 */
    SMRAM_FIELD_P5_TR_ACCESS,        /* 1A4 */
    SMRAM_FIELD_P5_TR_BASE,          /* 1A0 */
    SMRAM_FIELD_P5_TR_LIMIT,         /* 19C */
    SMRAM_FIELD_P5_IDTR_ACCESS,      /* 198 */
    SMRAM_FIELD_P5_IDTR_BASE,        /* 194 */
    SMRAM_FIELD_P5_IDTR_LIMIT,       /* 190 */
    SMRAM_FIELD_P5_GDTR_ACCESS,      /* 18C */
    SMRAM_FIELD_P5_GDTR_BASE,        /* 188 */
    SMRAM_FIELD_P5_GDTR_LIMIT,       /* 184 */
    SMRAM_FIELD_P5_LDTR_ACCESS,      /* 180 */
    SMRAM_FIELD_P5_LDTR_BASE,        /* 17C */
    SMRAM_FIELD_P5_LDTR_LIMIT,       /* 178 */
    SMRAM_FIELD_P5_GS_ACCESS,        /* 174 */
    SMRAM_FIELD_P5_GS_BASE,          /* 170 */
    SMRAM_FIELD_P5_GS_LIMIT,         /* 16C */
    SMRAM_FIELD_P5_FS_ACCESS,        /* 168 */
    SMRAM_FIELD_P5_FS_BASE,          /* 164 */
    SMRAM_FIELD_P5_FS_LIMIT,         /* 160 */
    SMRAM_FIELD_P5_DS_ACCESS,        /* 15C */
    SMRAM_FIELD_P5_DS_BASE,          /* 158 */
    SMRAM_FIELD_P5_DS_LIMIT,         /* 154 */
    SMRAM_FIELD_P5_SS_ACCESS,        /* 150 */
    SMRAM_FIELD_P5_SS_BASE,          /* 14C */
    SMRAM_FIELD_P5_SS_LIMIT,         /* 148 */
    SMRAM_FIELD_P5_CS_ACCESS,        /* 144 */
    SMRAM_FIELD_P5_CS_BASE,          /* 140 */
    SMRAM_FIELD_P5_CS_LIMIT,         /* 13C */
    SMRAM_FIELD_P5_ES_ACCESS,        /* 138 */
    SMRAM_FIELD_P5_ES_BASE,          /* 134 */
    SMRAM_FIELD_P5_ES_LIMIT,         /* 130 */
    SMRAM_FIELD_P5_UNWRITTEN_1,      /* 12C */
    SMRAM_FIELD_P5_CR4,              /* 128 */
    SMRAM_FIELD_P5_ALTERNATE_DR6,    /* 124 */
    SMRAM_FIELD_P5_RESERVED_1,       /* 120 */
    SMRAM_FIELD_P5_RESERVED_2,       /* 11C */
    SMRAM_FIELD_P5_RESERVED_3,       /* 118 */
    SMRAM_FIELD_P5_RESERVED_4,       /* 114 */
    SMRAM_FIELD_P5_IO_RESTART_EIP,   /* 110 */
    SMRAM_FIELD_P5_IO_RESTART_ESI,   /* 10C */
    SMRAM_FIELD_P5_IO_RESTART_ECX,   /* 108 */
    SMRAM_FIELD_P5_IO_RESTART_EDI,   /* 104 */
    SMRAM_FIELD_P5_AUTOHALT_RESTART, /* 100 */
    SMRAM_FIELD_P5_SMM_REVISION_ID,  /* 0FC */
    SMRAM_FIELD_P5_SMBASE_OFFSET,    /* 0F8 */
    SMRAM_FIELD_AM486_CR2,           /* 0F4 */
    SMRAM_FIELD_AM486_DR0,           /* 0F0 */
    SMRAM_FIELD_AM486_DR1,           /* 0EC */
    SMRAM_FIELD_AM486_DR2,           /* 0E8 */
    SMRAM_FIELD_AM486_DR3,           /* 0E4 */
    SMRAM_FIELD_P5_LAST
};

enum SMMRAM_Fields_P6 {
    SMRAM_FIELD_P6_CR0 = 0,          /* 1FC */
    SMRAM_FIELD_P6_CR3,              /* 1F8 */
    SMRAM_FIELD_P6_EFLAGS,           /* 1F4 */
    SMRAM_FIELD_P6_EIP,              /* 1F0 */
    SMRAM_FIELD_P6_EDI,              /* 1EC */
    SMRAM_FIELD_P6_ESI,              /* 1E8 */
    SMRAM_FIELD_P6_EBP,              /* 1E4 */
    SMRAM_FIELD_P6_ESP,              /* 1E0 */
    SMRAM_FIELD_P6_EBX,              /* 1DC */
    SMRAM_FIELD_P6_EDX,              /* 1D8 */
    SMRAM_FIELD_P6_ECX,              /* 1D4 */
    SMRAM_FIELD_P6_EAX,              /* 1D0 */
    SMRAM_FIELD_P6_DR6,              /* 1CC */
    SMRAM_FIELD_P6_DR7,              /* 1C8 */
    SMRAM_FIELD_P6_TR_SELECTOR,      /* 1C4 */
    SMRAM_FIELD_P6_LDTR_SELECTOR,    /* 1C0 */
    SMRAM_FIELD_P6_GS_SELECTOR,      /* 1BC */
    SMRAM_FIELD_P6_FS_SELECTOR,      /* 1B8 */
    SMRAM_FIELD_P6_DS_SELECTOR,      /* 1B4 */
    SMRAM_FIELD_P6_SS_SELECTOR,      /* 1B0 */
    SMRAM_FIELD_P6_CS_SELECTOR,      /* 1AC */
    SMRAM_FIELD_P6_ES_SELECTOR,      /* 1A8 */
    SMRAM_FIELD_P6_SS_BASE,          /* 1A4 */
    SMRAM_FIELD_P6_SS_LIMIT,         /* 1A0 */
    SMRAM_FIELD_P6_SS_SELECTOR_AR,   /* 19C */
    SMRAM_FIELD_P6_CS_BASE,          /* 198 */
    SMRAM_FIELD_P6_CS_LIMIT,         /* 194 */
    SMRAM_FIELD_P6_CS_SELECTOR_AR,   /* 190 */
    SMRAM_FIELD_P6_ES_BASE,          /* 18C */
    SMRAM_FIELD_P6_ES_LIMIT,         /* 188 */
    SMRAM_FIELD_P6_ES_SELECTOR_AR,   /* 184 */
    SMRAM_FIELD_P6_LDTR_BASE,        /* 180 */
    SMRAM_FIELD_P6_LDTR_LIMIT,       /* 17C */
    SMRAM_FIELD_P6_LDTR_SELECTOR_AR, /* 178 */
    SMRAM_FIELD_P6_GDTR_BASE,        /* 174 */
    SMRAM_FIELD_P6_GDTR_LIMIT,       /* 170 */
    SMRAM_FIELD_P6_GDTR_SELECTOR_AR, /* 16C */
    SMRAM_FIELD_P6_SREG_STATUS1,     /* 168 */
    SMRAM_FIELD_P6_TR_BASE,          /* 164 */
    SMRAM_FIELD_P6_TR_LIMIT,         /* 160 */
    SMRAM_FIELD_P6_TR_SELECTOR_AR,   /* 15C */
    SMRAM_FIELD_P6_IDTR_BASE,        /* 158 */
    SMRAM_FIELD_P6_IDTR_LIMIT,       /* 154 */
    SMRAM_FIELD_P6_IDTR_SELECTOR_AR, /* 150 */
    SMRAM_FIELD_P6_GS_BASE,          /* 14C */
    SMRAM_FIELD_P6_GS_LIMIT,         /* 148 */
    SMRAM_FIELD_P6_GS_SELECTOR_AR,   /* 144 */
    SMRAM_FIELD_P6_FS_BASE,          /* 140 */
    SMRAM_FIELD_P6_FS_LIMIT,         /* 13C */
    SMRAM_FIELD_P6_FS_SELECTOR_AR,   /* 138 */
    SMRAM_FIELD_P6_DS_BASE,          /* 134 */
    SMRAM_FIELD_P6_DS_LIMIT,         /* 130 */
    SMRAM_FIELD_P6_DS_SELECTOR_AR,   /* 12C */
    SMRAM_FIELD_P6_SREG_STATUS0,     /* 128 */
    SMRAM_FIELD_P6_ALTERNATIVE_DR6,  /* 124 */
    SMRAM_FIELD_P6_CPL,              /* 120 */
    SMRAM_FIELD_P6_SMM_STATUS,       /* 11C */
    SMRAM_FIELD_P6_A20M,             /* 118 */
    SMRAM_FIELD_P6_CR4,              /* 114 */
    SMRAM_FIELD_P6_IO_RESTART_EIP,   /* 110 */
    SMRAM_FIELD_P6_IO_RESTART_ESI,   /* 10C */
    SMRAM_FIELD_P6_IO_RESTART_ECX,   /* 108 */
    SMRAM_FIELD_P6_IO_RESTART_EDI,   /* 104 */
    SMRAM_FIELD_P6_AUTOHALT_RESTART, /* 100 */
    SMRAM_FIELD_P6_SMM_REVISION_ID,  /* 0FC */
    SMRAM_FIELD_P6_SMBASE_OFFSET,    /* 0F8 */
    SMRAM_FIELD_P6_LAST
};

enum SMMRAM_Fields_AMD_K {
    SMRAM_FIELD_AMD_K_CR0 = 0,          /* 1FC */
    SMRAM_FIELD_AMD_K_CR3,              /* 1F8 */
    SMRAM_FIELD_AMD_K_EFLAGS,           /* 1F4 */
    SMRAM_FIELD_AMD_K_EIP,              /* 1F0 */
    SMRAM_FIELD_AMD_K_EDI,              /* 1EC */
    SMRAM_FIELD_AMD_K_ESI,              /* 1E8 */
    SMRAM_FIELD_AMD_K_EBP,              /* 1E4 */
    SMRAM_FIELD_AMD_K_ESP,              /* 1E0 */
    SMRAM_FIELD_AMD_K_EBX,              /* 1DC */
    SMRAM_FIELD_AMD_K_EDX,              /* 1D8 */
    SMRAM_FIELD_AMD_K_ECX,              /* 1D4 */
    SMRAM_FIELD_AMD_K_EAX,              /* 1D0 */
    SMRAM_FIELD_AMD_K_DR6,              /* 1CC */
    SMRAM_FIELD_AMD_K_DR7,              /* 1C8 */
    SMRAM_FIELD_AMD_K_TR_SELECTOR,      /* 1C4 */
    SMRAM_FIELD_AMD_K_LDTR_SELECTOR,    /* 1C0 */
    SMRAM_FIELD_AMD_K_GS_SELECTOR,      /* 1BC */
    SMRAM_FIELD_AMD_K_FS_SELECTOR,      /* 1B8 */
    SMRAM_FIELD_AMD_K_DS_SELECTOR,      /* 1B4 */
    SMRAM_FIELD_AMD_K_SS_SELECTOR,      /* 1B0 */
    SMRAM_FIELD_AMD_K_CS_SELECTOR,      /* 1AC */
    SMRAM_FIELD_AMD_K_ES_SELECTOR,      /* 1A8 */
    SMRAM_FIELD_AMD_K_IO_RESTART_DWORD, /* 1A4 */
    SMRAM_FIELD_AMD_K_RESERVED_1,       /* 1A0 */
    SMRAM_FIELD_AMD_K_IO_RESTART_EIP,   /* 19C */
    SMRAM_FIELD_AMD_K_RESERVED_2,       /* 198 */
    SMRAM_FIELD_AMD_K_RESERVED_3,       /* 194 */
    SMRAM_FIELD_AMD_K_IDTR_BASE,        /* 190 */
    SMRAM_FIELD_AMD_K_IDTR_LIMIT,       /* 18C */
    SMRAM_FIELD_AMD_K_GDTR_BASE,        /* 188 */
    SMRAM_FIELD_AMD_K_GDTR_LIMIT,       /* 184 */
    SMRAM_FIELD_AMD_K_TR_ACCESS,        /* 180 */
    SMRAM_FIELD_AMD_K_TR_BASE,          /* 17C */
    SMRAM_FIELD_AMD_K_TR_LIMIT,         /* 178 */
    SMRAM_FIELD_AMD_K_LDTR_ACCESS,      /* 174 - reserved on K6 */
    SMRAM_FIELD_AMD_K_LDTR_BASE,        /* 170 */
    SMRAM_FIELD_AMD_K_LDTR_LIMIT,       /* 16C */
    SMRAM_FIELD_AMD_K_GS_ACCESS,        /* 168 */
    SMRAM_FIELD_AMD_K_GS_BASE,          /* 164 */
    SMRAM_FIELD_AMD_K_GS_LIMIT,         /* 160 */
    SMRAM_FIELD_AMD_K_FS_ACCESS,        /* 15C */
    SMRAM_FIELD_AMD_K_FS_BASE,          /* 158 */
    SMRAM_FIELD_AMD_K_FS_LIMIT,         /* 154 */
    SMRAM_FIELD_AMD_K_DS_ACCESS,        /* 150 */
    SMRAM_FIELD_AMD_K_DS_BASE,          /* 14C */
    SMRAM_FIELD_AMD_K_DS_LIMIT,         /* 148 */
    SMRAM_FIELD_AMD_K_SS_ACCESS,        /* 144 */
    SMRAM_FIELD_AMD_K_SS_BASE,          /* 140 */
    SMRAM_FIELD_AMD_K_SS_LIMIT,         /* 13C */
    SMRAM_FIELD_AMD_K_CS_ACCESS,        /* 138 */
    SMRAM_FIELD_AMD_K_CS_BASE,          /* 134 */
    SMRAM_FIELD_AMD_K_CS_LIMIT,         /* 130 */
    SMRAM_FIELD_AMD_K_ES_ACCESS,        /* 12C */
    SMRAM_FIELD_AMD_K_ES_BASE,          /* 128 */
    SMRAM_FIELD_AMD_K_ES_LIMIT,         /* 124 */
    SMRAM_FIELD_AMD_K_RESERVED_4,       /* 120 */
    SMRAM_FIELD_AMD_K_RESERVED_5,       /* 11C */
    SMRAM_FIELD_AMD_K_RESERVED_6,       /* 118 */
    SMRAM_FIELD_AMD_K_CR2,              /* 114 */
    SMRAM_FIELD_AMD_K_CR4,              /* 110 */
    SMRAM_FIELD_AMD_K_IO_RESTART_ESI,   /* 10C */
    SMRAM_FIELD_AMD_K_IO_RESTART_ECX,   /* 108 */
    SMRAM_FIELD_AMD_K_IO_RESTART_EDI,   /* 104 */
    SMRAM_FIELD_AMD_K_AUTOHALT_RESTART, /* 100 */
    SMRAM_FIELD_AMD_K_SMM_REVISION_ID,  /* 0FC */
    SMRAM_FIELD_AMD_K_SMBASE_OFFSET,    /* 0F8 */
    SMRAM_FIELD_AMD_K_LAST
};

#ifdef ENABLE_386_COMMON_LOG
int x386_common_do_log = ENABLE_386_COMMON_LOG;

void
x386_common_log(const char *fmt, ...)
{
    va_list ap;

    if (x386_common_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define x386_common_log(fmt, ...)
#endif

int
is_lock_legal(uint32_t fetchdat)
{
    int legal = 1;

    if (is386) {
        fetch_dat_t fetch_dat;
        fetch_dat.fd = fetchdat;

        legal = lock_legal[fetch_dat.b[0]];
        if (legal == 1)
            legal = 1; // ((fetch_dat.b[1] >> 6) != 0x03);    /* reg is illegal */
        else if (legal == 2) {
            legal = lock_legal_0f[fetch_dat.b[1]];
            if (legal == 1)
                legal = ((fetch_dat.b[2] >> 6) != 0x03);    /* reg,reg is illegal */
            else if (legal == 3) {
                legal = lock_legal_ba[(fetch_dat.b[2] >> 3) & 0x07];
                if (legal == 1)
                    legal = ((fetch_dat.b[2] >> 6) != 0x03);    /* reg,imm is illegal */
            }
        } else if (legal == 3)  switch(fetch_dat.b[0]) {
            case 0x80 ... 0x83:
                legal = lock_legal_80[(fetch_dat.b[1] >> 3) & 0x07];
            if (legal == 1)
                legal = ((fetch_dat.b[1] >> 6) != 0x03);    /* reg is illegal */
            break;
            case 0xf6 ... 0xf7:
                legal = lock_legal_f6[(fetch_dat.b[1] >> 3) & 0x07];
            if (legal == 1)
                legal = ((fetch_dat.b[1] >> 6) != 0x03);    /* reg is illegal */
            break;
            case 0xfe ... 0xff:
                legal = lock_legal_fe[(fetch_dat.b[1] >> 3) & 0x07];
            if (legal == 1)
                legal = ((fetch_dat.b[1] >> 6) != 0x03);    /* reg is illegal */
            break;
            default:
                legal = 0;
            break;
        }
    }

    return legal;
}

/*Prefetch emulation is a fairly simplistic model:
  - All instruction bytes must be fetched before it starts.
  - Cycles used for non-instruction memory accesses are counted and subtracted
    from the total cycles taken
  - Any remaining cycles are used to refill the prefetch queue.

  Note that this is only used for 286 / 386 systems. It is disabled when the
  internal cache on 486+ CPUs is enabled.
*/
static int prefetch_bytes = 0;

void
prefetch_run(int instr_cycles, int bytes, int modrm, int reads, int reads_l, int writes, int writes_l, int ea32)
{
    int mem_cycles = reads * cpu_cycles_read + reads_l * cpu_cycles_read_l + writes * cpu_cycles_write + writes_l * cpu_cycles_write_l;

    if (instr_cycles < mem_cycles)
        instr_cycles = mem_cycles;

    prefetch_bytes -= prefetch_prefixes;
    prefetch_bytes -= bytes;
    if (modrm != -1) {
        if (ea32) {
            if ((modrm & 7) == 4) {
                if ((modrm & 0x700) == 0x500)
                    prefetch_bytes -= 5;
                else if ((modrm & 0xc0) == 0x40)
                    prefetch_bytes -= 2;
                else if ((modrm & 0xc0) == 0x80)
                    prefetch_bytes -= 5;
            } else {
                if ((modrm & 0xc7) == 0x05)
                    prefetch_bytes -= 4;
                else if ((modrm & 0xc0) == 0x40)
                    prefetch_bytes--;
                else if ((modrm & 0xc0) == 0x80)
                    prefetch_bytes -= 4;
            }
        } else {
            if ((modrm & 0xc7) == 0x06)
                prefetch_bytes -= 2;
            else if ((modrm & 0xc0) != 0xc0)
                prefetch_bytes -= ((modrm & 0xc0) >> 6);
        }
    }

    /* Fill up prefetch queue */
    while (prefetch_bytes < 0) {
        prefetch_bytes += cpu_prefetch_width;
        cycles -= cpu_prefetch_cycles;
    }

    /* Subtract cycles used for memory access by instruction */
    instr_cycles -= mem_cycles;

    while (instr_cycles >= cpu_prefetch_cycles) {
        prefetch_bytes += cpu_prefetch_width;
        instr_cycles -= cpu_prefetch_cycles;
    }

    prefetch_prefixes = 0;
    if (prefetch_bytes > 16)
        prefetch_bytes = 16;
}

void
prefetch_flush(void)
{
    prefetch_bytes = 0;
}

static __inline void
set_stack32(int s)
{
    if ((cr0 & 1) && !(cpu_state.eflags & VM_FLAG))
        stack32 = s;
    else
        stack32 = 0;

    if (stack32)
        cpu_cur_status |= CPU_STATUS_STACK32;
    else
        cpu_cur_status &= ~CPU_STATUS_STACK32;
}

static __inline void
set_use32(int u)
{
    if ((cr0 & 1) && !(cpu_state.eflags & VM_FLAG))
        use32 = u ? 0x300 : 0;
    else
        use32 = 0;

    if (use32)
        cpu_cur_status |= CPU_STATUS_USE32;
    else
        cpu_cur_status &= ~CPU_STATUS_USE32;
}

static void
smm_seg_load(x86seg *s)
{
    if (!is386)
        s->base &= 0x00ffffff;

    if ((s->access & 0x18) != 0x10 || !(s->access & (1 << 2))) {
        /* Expand down. */
        s->limit_high = s->limit;
        s->limit_low  = 0;
    } else {
        s->limit_high = (s->ar_high & 0x40) ? 0xffffffff : 0xffff;
        s->limit_low  = s->limit + 1;
    }

    if ((cr0 & 1) && !(cpu_state.eflags & VM_FLAG))
        s->checked = s->seg ? 1 : 0;
    else
        s->checked = 1;

    if (s == &cpu_state.seg_cs)
        set_use32(s->ar_high & 0x40);

    if (s == &cpu_state.seg_ds) {
        if (s->base == 0 && s->limit_low == 0 && s->limit_high == 0xffffffff)
            cpu_cur_status &= ~CPU_STATUS_NOTFLATDS;
        else
            cpu_cur_status |= CPU_STATUS_NOTFLATDS;
    }

    if (s == &cpu_state.seg_ss) {
        if (s->base == 0 && s->limit_low == 0 && s->limit_high == 0xffffffff)
            cpu_cur_status &= ~CPU_STATUS_NOTFLATSS;
        else
            cpu_cur_status |= CPU_STATUS_NOTFLATSS;
        set_stack32((s->ar_high & 0x40) ? 1 : 0);
    }
}

static void
smram_save_state_p5(uint32_t *saved_state, int in_hlt)
{
    saved_state[SMRAM_FIELD_P5_SMM_REVISION_ID] = SMM_REVISION_ID;
    saved_state[SMRAM_FIELD_P5_SMBASE_OFFSET]   = smbase;

    for (uint8_t n = 0; n < 8; n++)
        saved_state[SMRAM_FIELD_P5_EAX - n] = cpu_state.regs[n].l;

    if (in_hlt)
        saved_state[SMRAM_FIELD_P5_AUTOHALT_RESTART] = 1;
    else
        saved_state[SMRAM_FIELD_P5_AUTOHALT_RESTART] = 0;

    saved_state[SMRAM_FIELD_P5_EIP] = cpu_state.pc;

    saved_state[SMRAM_FIELD_P5_EFLAGS] = (cpu_state.eflags << 16) | (cpu_state.flags);

    saved_state[SMRAM_FIELD_P5_CR0] = cr0;
    saved_state[SMRAM_FIELD_P5_CR3] = cr3;
    saved_state[SMRAM_FIELD_P5_CR4] = cr4;
    saved_state[SMRAM_FIELD_P5_DR6] = dr[6];
    saved_state[SMRAM_FIELD_P5_DR7] = dr[7];

    /* TR */
    saved_state[SMRAM_FIELD_P5_TR_SELECTOR] = tr.seg;
    saved_state[SMRAM_FIELD_P5_TR_BASE]     = tr.base;
    saved_state[SMRAM_FIELD_P5_TR_LIMIT]    = tr.limit;
    saved_state[SMRAM_FIELD_P5_TR_ACCESS]   = (tr.ar_high << 16) | (tr.access << 8);

    /* LDTR */
    saved_state[SMRAM_FIELD_P5_LDTR_SELECTOR] = ldt.seg;
    saved_state[SMRAM_FIELD_P5_LDTR_BASE]     = ldt.base;
    saved_state[SMRAM_FIELD_P5_LDTR_LIMIT]    = ldt.limit;
    saved_state[SMRAM_FIELD_P5_LDTR_ACCESS]   = (ldt.ar_high << 16) | (ldt.access << 8);

    /* IDTR */
    saved_state[SMRAM_FIELD_P5_IDTR_BASE]   = idt.base;
    saved_state[SMRAM_FIELD_P5_IDTR_LIMIT]  = idt.limit;
    saved_state[SMRAM_FIELD_P5_IDTR_ACCESS] = (idt.ar_high << 16) | (idt.access << 8);

    /* GDTR */
    saved_state[SMRAM_FIELD_P5_GDTR_BASE]   = gdt.base;
    saved_state[SMRAM_FIELD_P5_GDTR_LIMIT]  = gdt.limit;
    saved_state[SMRAM_FIELD_P5_GDTR_ACCESS] = (gdt.ar_high << 16) | (gdt.access << 8);

    /* ES */
    saved_state[SMRAM_FIELD_P5_ES_SELECTOR] = cpu_state.seg_es.seg;
    saved_state[SMRAM_FIELD_P5_ES_BASE]     = cpu_state.seg_es.base;
    saved_state[SMRAM_FIELD_P5_ES_LIMIT]    = cpu_state.seg_es.limit;
    saved_state[SMRAM_FIELD_P5_ES_ACCESS]   = (cpu_state.seg_es.ar_high << 16) | (cpu_state.seg_es.access << 8);

    /* CS */
    saved_state[SMRAM_FIELD_P5_CS_SELECTOR] = cpu_state.seg_cs.seg;
    saved_state[SMRAM_FIELD_P5_CS_BASE]     = cpu_state.seg_cs.base;
    saved_state[SMRAM_FIELD_P5_CS_LIMIT]    = cpu_state.seg_cs.limit;
    saved_state[SMRAM_FIELD_P5_CS_ACCESS]   = (cpu_state.seg_cs.ar_high << 16) | (cpu_state.seg_cs.access << 8);

    /* DS */
    saved_state[SMRAM_FIELD_P5_DS_SELECTOR] = cpu_state.seg_ds.seg;
    saved_state[SMRAM_FIELD_P5_DS_BASE]     = cpu_state.seg_ds.base;
    saved_state[SMRAM_FIELD_P5_DS_LIMIT]    = cpu_state.seg_ds.limit;
    saved_state[SMRAM_FIELD_P5_DS_ACCESS]   = (cpu_state.seg_ds.ar_high << 16) | (cpu_state.seg_ds.access << 8);

    /* SS */
    saved_state[SMRAM_FIELD_P5_SS_SELECTOR] = cpu_state.seg_ss.seg;
    saved_state[SMRAM_FIELD_P5_SS_BASE]     = cpu_state.seg_ss.base;
    saved_state[SMRAM_FIELD_P5_SS_LIMIT]    = cpu_state.seg_ss.limit;
    saved_state[SMRAM_FIELD_P5_SS_ACCESS]   = (cpu_state.seg_ss.ar_high << 16) | (cpu_state.seg_ss.access << 8);

    /* FS */
    saved_state[SMRAM_FIELD_P5_FS_SELECTOR] = cpu_state.seg_fs.seg;
    saved_state[SMRAM_FIELD_P5_FS_BASE]     = cpu_state.seg_fs.base;
    saved_state[SMRAM_FIELD_P5_FS_LIMIT]    = cpu_state.seg_fs.limit;
    saved_state[SMRAM_FIELD_P5_FS_ACCESS]   = (cpu_state.seg_fs.ar_high << 16) | (cpu_state.seg_fs.access << 8);

    /* GS */
    saved_state[SMRAM_FIELD_P5_GS_SELECTOR] = cpu_state.seg_gs.seg;
    saved_state[SMRAM_FIELD_P5_GS_BASE]     = cpu_state.seg_gs.base;
    saved_state[SMRAM_FIELD_P5_GS_LIMIT]    = cpu_state.seg_gs.limit;
    saved_state[SMRAM_FIELD_P5_GS_ACCESS]   = (cpu_state.seg_gs.ar_high << 16) | (cpu_state.seg_gs.access << 8);

    /* Am486/5x86 stuff */
    if (!is_pentium) {
        saved_state[SMRAM_FIELD_AM486_CR2] = cr2;
        saved_state[SMRAM_FIELD_AM486_DR0] = dr[0];
        saved_state[SMRAM_FIELD_AM486_DR1] = dr[1];
        saved_state[SMRAM_FIELD_AM486_DR2] = dr[2];
        saved_state[SMRAM_FIELD_AM486_DR3] = dr[3];
    }
}

static void
smram_restore_state_p5(uint32_t *saved_state)
{
    for (uint8_t n = 0; n < 8; n++)
        cpu_state.regs[n].l = saved_state[SMRAM_FIELD_P5_EAX - n];

    if (saved_state[SMRAM_FIELD_P5_AUTOHALT_RESTART] & 0xffff)
        cpu_state.pc = saved_state[SMRAM_FIELD_P5_EIP] - 1;
    else
        cpu_state.pc = saved_state[SMRAM_FIELD_P5_EIP];

    cpu_state.eflags = saved_state[SMRAM_FIELD_P5_EFLAGS] >> 16;
    cpu_state.flags  = saved_state[SMRAM_FIELD_P5_EFLAGS] & 0xffff;

    cr0   = saved_state[SMRAM_FIELD_P5_CR0];
    cr3   = saved_state[SMRAM_FIELD_P5_CR3];
    cr4   = saved_state[SMRAM_FIELD_P5_CR4];
    dr[6] = saved_state[SMRAM_FIELD_P5_DR6];
    dr[7] = saved_state[SMRAM_FIELD_P5_DR7];

    /* TR */
    tr.seg     = saved_state[SMRAM_FIELD_P5_TR_SELECTOR];
    tr.base    = saved_state[SMRAM_FIELD_P5_TR_BASE];
    tr.limit   = saved_state[SMRAM_FIELD_P5_TR_LIMIT];
    tr.access  = (saved_state[SMRAM_FIELD_P5_TR_ACCESS] >> 8) & 0xff;
    tr.ar_high = (saved_state[SMRAM_FIELD_P5_TR_ACCESS] >> 16) & 0xff;
    smm_seg_load(&tr);

    /* LDTR */
    ldt.seg     = saved_state[SMRAM_FIELD_P5_LDTR_SELECTOR];
    ldt.base    = saved_state[SMRAM_FIELD_P5_LDTR_BASE];
    ldt.limit   = saved_state[SMRAM_FIELD_P5_LDTR_LIMIT];
    ldt.access  = (saved_state[SMRAM_FIELD_P5_LDTR_ACCESS] >> 8) & 0xff;
    ldt.ar_high = (saved_state[SMRAM_FIELD_P5_LDTR_ACCESS] >> 16) & 0xff;
    smm_seg_load(&ldt);

    /* IDTR */
    idt.base    = saved_state[SMRAM_FIELD_P5_IDTR_BASE];
    idt.limit   = saved_state[SMRAM_FIELD_P5_IDTR_LIMIT];
    idt.access  = (saved_state[SMRAM_FIELD_P5_IDTR_ACCESS] >> 8) & 0xff;
    idt.ar_high = (saved_state[SMRAM_FIELD_P5_IDTR_ACCESS] >> 16) & 0xff;

    /* GDTR */
    gdt.base    = saved_state[SMRAM_FIELD_P5_GDTR_BASE];
    gdt.limit   = saved_state[SMRAM_FIELD_P5_GDTR_LIMIT];
    gdt.access  = (saved_state[SMRAM_FIELD_P5_GDTR_ACCESS] >> 8) & 0xff;
    gdt.ar_high = (saved_state[SMRAM_FIELD_P5_GDTR_ACCESS] >> 16) & 0xff;

    /* ES */
    cpu_state.seg_es.seg     = saved_state[SMRAM_FIELD_P5_ES_SELECTOR];
    cpu_state.seg_es.base    = saved_state[SMRAM_FIELD_P5_ES_BASE];
    cpu_state.seg_es.limit   = saved_state[SMRAM_FIELD_P5_ES_LIMIT];
    cpu_state.seg_es.access  = (saved_state[SMRAM_FIELD_P5_ES_ACCESS] >> 8) & 0xff;
    cpu_state.seg_es.ar_high = (saved_state[SMRAM_FIELD_P5_ES_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_es);

    /* CS */
    cpu_state.seg_cs.seg     = saved_state[SMRAM_FIELD_P5_CS_SELECTOR];
    cpu_state.seg_cs.base    = saved_state[SMRAM_FIELD_P5_CS_BASE];
    cpu_state.seg_cs.limit   = saved_state[SMRAM_FIELD_P5_CS_LIMIT];
    cpu_state.seg_cs.access  = (saved_state[SMRAM_FIELD_P5_CS_ACCESS] >> 8) & 0xff;
    cpu_state.seg_cs.ar_high = (saved_state[SMRAM_FIELD_P5_CS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_cs);

    /* DS */
    cpu_state.seg_ds.seg     = saved_state[SMRAM_FIELD_P5_DS_SELECTOR];
    cpu_state.seg_ds.base    = saved_state[SMRAM_FIELD_P5_DS_BASE];
    cpu_state.seg_ds.limit   = saved_state[SMRAM_FIELD_P5_DS_LIMIT];
    cpu_state.seg_ds.access  = (saved_state[SMRAM_FIELD_P5_DS_ACCESS] >> 8) & 0xff;
    cpu_state.seg_ds.ar_high = (saved_state[SMRAM_FIELD_P5_DS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_ds);

    /* SS */
    cpu_state.seg_ss.seg    = saved_state[SMRAM_FIELD_P5_SS_SELECTOR];
    cpu_state.seg_ss.base   = saved_state[SMRAM_FIELD_P5_SS_BASE];
    cpu_state.seg_ss.limit  = saved_state[SMRAM_FIELD_P5_SS_LIMIT];
    cpu_state.seg_ss.access = (saved_state[SMRAM_FIELD_P5_SS_ACCESS] >> 8) & 0xff;
    /* The actual CPL (DPL of CS) is overwritten with DPL of SS. */
    cpu_state.seg_cs.access  = (cpu_state.seg_cs.access & ~0x60) | (cpu_state.seg_ss.access & 0x60);
    cpu_state.seg_ss.ar_high = (saved_state[SMRAM_FIELD_P5_SS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_ss);

    /* FS */
    cpu_state.seg_fs.seg     = saved_state[SMRAM_FIELD_P5_FS_SELECTOR];
    cpu_state.seg_fs.base    = saved_state[SMRAM_FIELD_P5_FS_BASE];
    cpu_state.seg_fs.limit   = saved_state[SMRAM_FIELD_P5_FS_LIMIT];
    cpu_state.seg_fs.access  = (saved_state[SMRAM_FIELD_P5_FS_ACCESS] >> 8) & 0xff;
    cpu_state.seg_fs.ar_high = (saved_state[SMRAM_FIELD_P5_FS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_fs);

    /* GS */
    cpu_state.seg_gs.seg     = saved_state[SMRAM_FIELD_P5_GS_SELECTOR];
    cpu_state.seg_gs.base    = saved_state[SMRAM_FIELD_P5_GS_BASE];
    cpu_state.seg_gs.limit   = saved_state[SMRAM_FIELD_P5_GS_LIMIT];
    cpu_state.seg_gs.access  = (saved_state[SMRAM_FIELD_P5_GS_ACCESS] >> 8) & 0xff;
    cpu_state.seg_gs.ar_high = (saved_state[SMRAM_FIELD_P5_GS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_gs);

    if (SMM_REVISION_ID & SMM_SMBASE_RELOCATION)
        smbase = saved_state[SMRAM_FIELD_P5_SMBASE_OFFSET];

    /* Am486/5x86 stuff */
    if (!is_pentium) {
        cr2   = saved_state[SMRAM_FIELD_AM486_CR2];
        dr[0] = saved_state[SMRAM_FIELD_AM486_DR0];
        dr[1] = saved_state[SMRAM_FIELD_AM486_DR1];
        dr[2] = saved_state[SMRAM_FIELD_AM486_DR2];
        dr[3] = saved_state[SMRAM_FIELD_AM486_DR3];
    }
}

static void
smram_save_state_p6(uint32_t *saved_state, int in_hlt)
{
    saved_state[SMRAM_FIELD_P6_SMM_REVISION_ID] = SMM_REVISION_ID;
    saved_state[SMRAM_FIELD_P6_SMBASE_OFFSET]   = smbase;

    for (uint8_t n = 0; n < 8; n++)
        saved_state[SMRAM_FIELD_P6_EAX - n] = cpu_state.regs[n].l;

    if (in_hlt)
        saved_state[SMRAM_FIELD_P6_AUTOHALT_RESTART] = 1;
    else
        saved_state[SMRAM_FIELD_P6_AUTOHALT_RESTART] = 0;
    saved_state[SMRAM_FIELD_P6_EIP] = cpu_state.pc;

    saved_state[SMRAM_FIELD_P6_EFLAGS] = (cpu_state.eflags << 16) | (cpu_state.flags);

    saved_state[SMRAM_FIELD_P6_CR0]  = cr0;
    saved_state[SMRAM_FIELD_P6_CR3]  = cr3;
    saved_state[SMRAM_FIELD_P6_CR4]  = cr4;
    saved_state[SMRAM_FIELD_P6_DR6]  = dr[6];
    saved_state[SMRAM_FIELD_P6_DR7]  = dr[7];
    saved_state[SMRAM_FIELD_P6_CPL]  = CPL;
    saved_state[SMRAM_FIELD_P6_A20M] = !mem_a20_state;

    /* TR */
    saved_state[SMRAM_FIELD_P6_TR_SELECTOR]    = tr.seg;
    saved_state[SMRAM_FIELD_P6_TR_BASE]        = tr.base;
    saved_state[SMRAM_FIELD_P6_TR_LIMIT]       = tr.limit;
    saved_state[SMRAM_FIELD_P6_TR_SELECTOR_AR] = (tr.ar_high << 24) | (tr.access << 16) | tr.seg;

    /* LDTR */
    saved_state[SMRAM_FIELD_P6_LDTR_SELECTOR]    = ldt.seg;
    saved_state[SMRAM_FIELD_P6_LDTR_BASE]        = ldt.base;
    saved_state[SMRAM_FIELD_P6_LDTR_LIMIT]       = ldt.limit;
    saved_state[SMRAM_FIELD_P6_LDTR_SELECTOR_AR] = (ldt.ar_high << 24) | (ldt.access << 16) | ldt.seg;

    /* IDTR */
    saved_state[SMRAM_FIELD_P6_IDTR_BASE]        = idt.base;
    saved_state[SMRAM_FIELD_P6_IDTR_LIMIT]       = idt.limit;
    saved_state[SMRAM_FIELD_P6_IDTR_SELECTOR_AR] = (idt.ar_high << 24) | (idt.access << 16) | idt.seg;

    /* GDTR */
    saved_state[SMRAM_FIELD_P6_GDTR_BASE]        = gdt.base;
    saved_state[SMRAM_FIELD_P6_GDTR_LIMIT]       = gdt.limit;
    saved_state[SMRAM_FIELD_P6_GDTR_SELECTOR_AR] = (gdt.ar_high << 24) | (gdt.access << 16) | gdt.seg;

    /* ES */
    saved_state[SMRAM_FIELD_P6_ES_SELECTOR]    = cpu_state.seg_es.seg;
    saved_state[SMRAM_FIELD_P6_ES_BASE]        = cpu_state.seg_es.base;
    saved_state[SMRAM_FIELD_P6_ES_LIMIT]       = cpu_state.seg_es.limit;
    saved_state[SMRAM_FIELD_P6_ES_SELECTOR_AR] = (cpu_state.seg_es.ar_high << 24) | (cpu_state.seg_es.access << 16) | cpu_state.seg_es.seg;

    /* CS */
    saved_state[SMRAM_FIELD_P6_CS_SELECTOR]    = cpu_state.seg_cs.seg;
    saved_state[SMRAM_FIELD_P6_CS_BASE]        = cpu_state.seg_cs.base;
    saved_state[SMRAM_FIELD_P6_CS_LIMIT]       = cpu_state.seg_cs.limit;
    saved_state[SMRAM_FIELD_P6_CS_SELECTOR_AR] = (cpu_state.seg_cs.ar_high << 24) | (cpu_state.seg_cs.access << 16) | cpu_state.seg_cs.seg;

    /* DS */
    saved_state[SMRAM_FIELD_P6_DS_SELECTOR]    = cpu_state.seg_ds.seg;
    saved_state[SMRAM_FIELD_P6_DS_BASE]        = cpu_state.seg_ds.base;
    saved_state[SMRAM_FIELD_P6_DS_LIMIT]       = cpu_state.seg_ds.limit;
    saved_state[SMRAM_FIELD_P6_DS_SELECTOR_AR] = (cpu_state.seg_ds.ar_high << 24) | (cpu_state.seg_ds.access << 16) | cpu_state.seg_ds.seg;

    /* SS */
    saved_state[SMRAM_FIELD_P6_SS_SELECTOR]    = cpu_state.seg_ss.seg;
    saved_state[SMRAM_FIELD_P6_SS_BASE]        = cpu_state.seg_ss.base;
    saved_state[SMRAM_FIELD_P6_SS_LIMIT]       = cpu_state.seg_ss.limit;
    saved_state[SMRAM_FIELD_P6_SS_SELECTOR_AR] = (cpu_state.seg_ss.ar_high << 24) | (cpu_state.seg_ss.access << 16) | cpu_state.seg_ss.seg;

    /* FS */
    saved_state[SMRAM_FIELD_P6_FS_SELECTOR]    = cpu_state.seg_fs.seg;
    saved_state[SMRAM_FIELD_P6_FS_BASE]        = cpu_state.seg_fs.base;
    saved_state[SMRAM_FIELD_P6_FS_LIMIT]       = cpu_state.seg_fs.limit;
    saved_state[SMRAM_FIELD_P6_FS_SELECTOR_AR] = (cpu_state.seg_fs.ar_high << 24) | (cpu_state.seg_fs.access << 16) | cpu_state.seg_fs.seg;

    /* GS */
    saved_state[SMRAM_FIELD_P6_GS_SELECTOR]    = cpu_state.seg_gs.seg;
    saved_state[SMRAM_FIELD_P6_GS_BASE]        = cpu_state.seg_gs.base;
    saved_state[SMRAM_FIELD_P6_GS_LIMIT]       = cpu_state.seg_gs.limit;
    saved_state[SMRAM_FIELD_P6_GS_SELECTOR_AR] = (cpu_state.seg_gs.ar_high << 24) | (cpu_state.seg_gs.access << 16) | cpu_state.seg_gs.seg;
}

static void
smram_restore_state_p6(uint32_t *saved_state)
{
    for (uint8_t n = 0; n < 8; n++)
        cpu_state.regs[n].l = saved_state[SMRAM_FIELD_P6_EAX - n];

    if (saved_state[SMRAM_FIELD_P6_AUTOHALT_RESTART] & 0xffff)
        cpu_state.pc = saved_state[SMRAM_FIELD_P6_EIP] - 1;
    else
        cpu_state.pc = saved_state[SMRAM_FIELD_P6_EIP];

    cpu_state.eflags = saved_state[SMRAM_FIELD_P6_EFLAGS] >> 16;
    cpu_state.flags  = saved_state[SMRAM_FIELD_P6_EFLAGS] & 0xffff;

    cr0   = saved_state[SMRAM_FIELD_P6_CR0];
    cr3   = saved_state[SMRAM_FIELD_P6_CR3];
    cr4   = saved_state[SMRAM_FIELD_P6_CR4];
    dr[6] = saved_state[SMRAM_FIELD_P6_DR6];
    dr[7] = saved_state[SMRAM_FIELD_P6_DR7];

    /* TR */
    tr.seg     = saved_state[SMRAM_FIELD_P6_TR_SELECTOR];
    tr.base    = saved_state[SMRAM_FIELD_P6_TR_BASE];
    tr.limit   = saved_state[SMRAM_FIELD_P6_TR_LIMIT];
    tr.access  = (saved_state[SMRAM_FIELD_P6_TR_SELECTOR_AR] >> 16) & 0xff;
    tr.ar_high = (saved_state[SMRAM_FIELD_P6_TR_SELECTOR_AR] >> 24) & 0xff;
    smm_seg_load(&tr);

    /* LDTR */
    ldt.seg     = saved_state[SMRAM_FIELD_P6_LDTR_SELECTOR];
    ldt.base    = saved_state[SMRAM_FIELD_P6_LDTR_BASE];
    ldt.limit   = saved_state[SMRAM_FIELD_P6_LDTR_LIMIT];
    ldt.access  = (saved_state[SMRAM_FIELD_P6_LDTR_SELECTOR_AR] >> 16) & 0xff;
    ldt.ar_high = (saved_state[SMRAM_FIELD_P6_LDTR_SELECTOR_AR] >> 24) & 0xff;
    smm_seg_load(&ldt);

    /* IDTR */
    idt.base    = saved_state[SMRAM_FIELD_P6_IDTR_BASE];
    idt.limit   = saved_state[SMRAM_FIELD_P6_IDTR_LIMIT];
    idt.access  = (saved_state[SMRAM_FIELD_P6_IDTR_SELECTOR_AR] >> 16) & 0xff;
    idt.ar_high = (saved_state[SMRAM_FIELD_P6_IDTR_SELECTOR_AR] >> 24) & 0xff;

    /* GDTR */
    gdt.base    = saved_state[SMRAM_FIELD_P6_GDTR_BASE];
    gdt.limit   = saved_state[SMRAM_FIELD_P6_GDTR_LIMIT];
    gdt.access  = (saved_state[SMRAM_FIELD_P6_GDTR_SELECTOR_AR] >> 16) & 0xff;
    gdt.ar_high = (saved_state[SMRAM_FIELD_P6_GDTR_SELECTOR_AR] >> 24) & 0xff;

    /* ES */
    cpu_state.seg_es.seg     = saved_state[SMRAM_FIELD_P6_ES_SELECTOR];
    cpu_state.seg_es.base    = saved_state[SMRAM_FIELD_P6_ES_BASE];
    cpu_state.seg_es.limit   = saved_state[SMRAM_FIELD_P6_ES_LIMIT];
    cpu_state.seg_es.access  = (saved_state[SMRAM_FIELD_P6_ES_SELECTOR_AR] >> 16) & 0xff;
    cpu_state.seg_es.ar_high = (saved_state[SMRAM_FIELD_P6_ES_SELECTOR_AR] >> 24) & 0xff;
    smm_seg_load(&cpu_state.seg_es);

    /* CS */
    cpu_state.seg_cs.seg     = saved_state[SMRAM_FIELD_P6_CS_SELECTOR];
    cpu_state.seg_cs.base    = saved_state[SMRAM_FIELD_P6_CS_BASE];
    cpu_state.seg_cs.limit   = saved_state[SMRAM_FIELD_P6_CS_LIMIT];
    cpu_state.seg_cs.access  = (saved_state[SMRAM_FIELD_P6_CS_SELECTOR_AR] >> 16) & 0xff;
    cpu_state.seg_cs.ar_high = (saved_state[SMRAM_FIELD_P6_CS_SELECTOR_AR] >> 24) & 0xff;
    smm_seg_load(&cpu_state.seg_cs);
    cpu_state.seg_cs.access = (cpu_state.seg_cs.access & ~0x60) | ((saved_state[SMRAM_FIELD_P6_CPL] & 0x03) << 5);

    /* DS */
    cpu_state.seg_ds.seg     = saved_state[SMRAM_FIELD_P6_DS_SELECTOR];
    cpu_state.seg_ds.base    = saved_state[SMRAM_FIELD_P6_DS_BASE];
    cpu_state.seg_ds.limit   = saved_state[SMRAM_FIELD_P6_DS_LIMIT];
    cpu_state.seg_ds.access  = (saved_state[SMRAM_FIELD_P6_DS_SELECTOR_AR] >> 16) & 0xff;
    cpu_state.seg_ds.ar_high = (saved_state[SMRAM_FIELD_P6_DS_SELECTOR_AR] >> 24) & 0xff;
    smm_seg_load(&cpu_state.seg_ds);

    /* SS */
    cpu_state.seg_ss.seg     = saved_state[SMRAM_FIELD_P6_SS_SELECTOR];
    cpu_state.seg_ss.base    = saved_state[SMRAM_FIELD_P6_SS_BASE];
    cpu_state.seg_ss.limit   = saved_state[SMRAM_FIELD_P6_SS_LIMIT];
    cpu_state.seg_ss.access  = (saved_state[SMRAM_FIELD_P6_SS_SELECTOR_AR] >> 16) & 0xff;
    cpu_state.seg_ss.ar_high = (saved_state[SMRAM_FIELD_P6_SS_SELECTOR_AR] >> 24) & 0xff;
    smm_seg_load(&cpu_state.seg_ss);

    /* FS */
    cpu_state.seg_fs.seg     = saved_state[SMRAM_FIELD_P6_FS_SELECTOR];
    cpu_state.seg_fs.base    = saved_state[SMRAM_FIELD_P6_FS_BASE];
    cpu_state.seg_fs.limit   = saved_state[SMRAM_FIELD_P6_FS_LIMIT];
    cpu_state.seg_fs.access  = (saved_state[SMRAM_FIELD_P6_FS_SELECTOR_AR] >> 16) & 0xff;
    cpu_state.seg_fs.ar_high = (saved_state[SMRAM_FIELD_P6_FS_SELECTOR_AR] >> 24) & 0xff;
    smm_seg_load(&cpu_state.seg_fs);

    /* GS */
    cpu_state.seg_gs.seg     = saved_state[SMRAM_FIELD_P6_GS_SELECTOR];
    cpu_state.seg_gs.base    = saved_state[SMRAM_FIELD_P6_GS_BASE];
    cpu_state.seg_gs.limit   = saved_state[SMRAM_FIELD_P6_GS_LIMIT];
    cpu_state.seg_gs.access  = (saved_state[SMRAM_FIELD_P6_GS_SELECTOR_AR] >> 16) & 0xff;
    cpu_state.seg_gs.ar_high = (saved_state[SMRAM_FIELD_P6_GS_SELECTOR_AR] >> 24) & 0xff;
    smm_seg_load(&cpu_state.seg_gs);

    rammask     = cpu_16bitbus ? 0xFFFFFF : 0xFFFFFFFF;
    if (is6117)
        rammask |= 0x3000000;

    if (saved_state[SMRAM_FIELD_P6_A20M] & 0x01)
        rammask &= 0xffefffff;

    flushmmucache();

    if (SMM_REVISION_ID & SMM_SMBASE_RELOCATION)
        smbase = saved_state[SMRAM_FIELD_P6_SMBASE_OFFSET];
}

static void
smram_save_state_amd_k(uint32_t *saved_state, int in_hlt)
{
    saved_state[SMRAM_FIELD_AMD_K_SMM_REVISION_ID] = SMM_REVISION_ID;
    saved_state[SMRAM_FIELD_AMD_K_SMBASE_OFFSET]   = smbase;

    for (uint8_t n = 0; n < 8; n++)
        saved_state[SMRAM_FIELD_AMD_K_EAX - n] = cpu_state.regs[n].l;

    if (in_hlt)
        saved_state[SMRAM_FIELD_AMD_K_AUTOHALT_RESTART] = 1;
    else
        saved_state[SMRAM_FIELD_AMD_K_AUTOHALT_RESTART] = 0;

    saved_state[SMRAM_FIELD_AMD_K_EIP] = cpu_state.pc;

    saved_state[SMRAM_FIELD_AMD_K_EFLAGS] = (cpu_state.eflags << 16) | (cpu_state.flags);

    saved_state[SMRAM_FIELD_AMD_K_CR0] = cr0;
    saved_state[SMRAM_FIELD_AMD_K_CR2] = cr2;
    saved_state[SMRAM_FIELD_AMD_K_CR3] = cr3;
    saved_state[SMRAM_FIELD_AMD_K_CR4] = cr4;
    saved_state[SMRAM_FIELD_AMD_K_DR6] = dr[6];
    saved_state[SMRAM_FIELD_AMD_K_DR7] = dr[7];

    /* TR */
    saved_state[SMRAM_FIELD_AMD_K_TR_SELECTOR] = tr.seg;
    saved_state[SMRAM_FIELD_AMD_K_TR_BASE]     = tr.base;
    saved_state[SMRAM_FIELD_AMD_K_TR_LIMIT]    = tr.limit;
    saved_state[SMRAM_FIELD_AMD_K_TR_ACCESS]   = (tr.ar_high << 16) | (tr.access << 8);

    /* LDTR */
    saved_state[SMRAM_FIELD_AMD_K_LDTR_SELECTOR] = ldt.seg;
    saved_state[SMRAM_FIELD_AMD_K_LDTR_BASE]     = ldt.base;
    saved_state[SMRAM_FIELD_AMD_K_LDTR_LIMIT]    = ldt.limit;
    if (!is_k6)
        saved_state[SMRAM_FIELD_AMD_K_LDTR_ACCESS] = (ldt.ar_high << 16) | (ldt.access << 8);

    /* IDTR */
    saved_state[SMRAM_FIELD_AMD_K_IDTR_BASE]  = idt.base;
    saved_state[SMRAM_FIELD_AMD_K_IDTR_LIMIT] = idt.limit;

    /* GDTR */
    saved_state[SMRAM_FIELD_AMD_K_GDTR_BASE]  = gdt.base;
    saved_state[SMRAM_FIELD_AMD_K_GDTR_LIMIT] = gdt.limit;

    /* ES */
    saved_state[SMRAM_FIELD_AMD_K_ES_SELECTOR] = cpu_state.seg_es.seg;
    saved_state[SMRAM_FIELD_AMD_K_ES_BASE]     = cpu_state.seg_es.base;
    saved_state[SMRAM_FIELD_AMD_K_ES_LIMIT]    = cpu_state.seg_es.limit;
    saved_state[SMRAM_FIELD_AMD_K_ES_ACCESS]   = (cpu_state.seg_es.ar_high << 16) | (cpu_state.seg_es.access << 8);

    /* CS */
    saved_state[SMRAM_FIELD_AMD_K_CS_SELECTOR] = cpu_state.seg_cs.seg;
    saved_state[SMRAM_FIELD_AMD_K_CS_BASE]     = cpu_state.seg_cs.base;
    saved_state[SMRAM_FIELD_AMD_K_CS_LIMIT]    = cpu_state.seg_cs.limit;
    saved_state[SMRAM_FIELD_AMD_K_CS_ACCESS]   = (cpu_state.seg_cs.ar_high << 16) | (cpu_state.seg_cs.access << 8);

    /* DS */
    saved_state[SMRAM_FIELD_AMD_K_DS_SELECTOR] = cpu_state.seg_ds.seg;
    saved_state[SMRAM_FIELD_AMD_K_DS_BASE]     = cpu_state.seg_ds.base;
    saved_state[SMRAM_FIELD_AMD_K_DS_LIMIT]    = cpu_state.seg_ds.limit;
    saved_state[SMRAM_FIELD_AMD_K_DS_ACCESS]   = (cpu_state.seg_ds.ar_high << 16) | (cpu_state.seg_ds.access << 8);

    /* SS */
    saved_state[SMRAM_FIELD_AMD_K_SS_SELECTOR] = cpu_state.seg_ss.seg;
    saved_state[SMRAM_FIELD_AMD_K_SS_BASE]     = cpu_state.seg_ss.base;
    saved_state[SMRAM_FIELD_AMD_K_SS_LIMIT]    = cpu_state.seg_ss.limit;
    saved_state[SMRAM_FIELD_AMD_K_SS_ACCESS]   = (cpu_state.seg_ss.ar_high << 16) | (cpu_state.seg_ss.access << 8);

    /* FS */
    saved_state[SMRAM_FIELD_AMD_K_FS_SELECTOR] = cpu_state.seg_fs.seg;
    saved_state[SMRAM_FIELD_AMD_K_FS_BASE]     = cpu_state.seg_fs.base;
    saved_state[SMRAM_FIELD_AMD_K_FS_LIMIT]    = cpu_state.seg_fs.limit;
    saved_state[SMRAM_FIELD_AMD_K_FS_ACCESS]   = (cpu_state.seg_fs.ar_high << 16) | (cpu_state.seg_fs.access << 8);

    /* GS */
    saved_state[SMRAM_FIELD_AMD_K_GS_SELECTOR] = cpu_state.seg_gs.seg;
    saved_state[SMRAM_FIELD_AMD_K_GS_BASE]     = cpu_state.seg_gs.base;
    saved_state[SMRAM_FIELD_AMD_K_GS_LIMIT]    = cpu_state.seg_gs.limit;
    saved_state[SMRAM_FIELD_AMD_K_GS_ACCESS]   = (cpu_state.seg_gs.ar_high << 16) | (cpu_state.seg_gs.access << 8);
}

static void
smram_restore_state_amd_k(uint32_t *saved_state)
{
    for (uint8_t n = 0; n < 8; n++)
        cpu_state.regs[n].l = saved_state[SMRAM_FIELD_AMD_K_EAX - n];

    if (saved_state[SMRAM_FIELD_AMD_K_AUTOHALT_RESTART] & 0xffff)
        cpu_state.pc = saved_state[SMRAM_FIELD_AMD_K_EIP] - 1;
    else
        cpu_state.pc = saved_state[SMRAM_FIELD_AMD_K_EIP];

    cpu_state.eflags = saved_state[SMRAM_FIELD_AMD_K_EFLAGS] >> 16;
    cpu_state.flags  = saved_state[SMRAM_FIELD_AMD_K_EFLAGS] & 0xffff;

    cr0   = saved_state[SMRAM_FIELD_AMD_K_CR0];
    cr2   = saved_state[SMRAM_FIELD_AMD_K_CR2];
    cr3   = saved_state[SMRAM_FIELD_AMD_K_CR3];
    cr4   = saved_state[SMRAM_FIELD_AMD_K_CR4];
    dr[6] = saved_state[SMRAM_FIELD_AMD_K_DR6];
    dr[7] = saved_state[SMRAM_FIELD_AMD_K_DR7];

    /* TR */
    tr.seg     = saved_state[SMRAM_FIELD_AMD_K_TR_SELECTOR];
    tr.base    = saved_state[SMRAM_FIELD_AMD_K_TR_BASE];
    tr.limit   = saved_state[SMRAM_FIELD_AMD_K_TR_LIMIT];
    tr.access  = (saved_state[SMRAM_FIELD_AMD_K_TR_ACCESS] >> 8) & 0xff;
    tr.ar_high = (saved_state[SMRAM_FIELD_AMD_K_TR_ACCESS] >> 16) & 0xff;
    smm_seg_load(&tr);

    /* LDTR */
    ldt.seg   = saved_state[SMRAM_FIELD_AMD_K_LDTR_SELECTOR];
    ldt.base  = saved_state[SMRAM_FIELD_AMD_K_LDTR_BASE];
    ldt.limit = saved_state[SMRAM_FIELD_AMD_K_LDTR_LIMIT];
    if (!is_k6) {
        ldt.access  = (saved_state[SMRAM_FIELD_AMD_K_LDTR_ACCESS] >> 8) & 0xff;
        ldt.ar_high = (saved_state[SMRAM_FIELD_AMD_K_LDTR_ACCESS] >> 16) & 0xff;
    }
    smm_seg_load(&ldt);

    /* IDTR */
    idt.base  = saved_state[SMRAM_FIELD_AMD_K_IDTR_BASE];
    idt.limit = saved_state[SMRAM_FIELD_AMD_K_IDTR_LIMIT];

    /* GDTR */
    gdt.base  = saved_state[SMRAM_FIELD_AMD_K_GDTR_BASE];
    gdt.limit = saved_state[SMRAM_FIELD_AMD_K_GDTR_LIMIT];

    /* ES */
    cpu_state.seg_es.seg     = saved_state[SMRAM_FIELD_AMD_K_ES_SELECTOR];
    cpu_state.seg_es.base    = saved_state[SMRAM_FIELD_AMD_K_ES_BASE];
    cpu_state.seg_es.limit   = saved_state[SMRAM_FIELD_AMD_K_ES_LIMIT];
    cpu_state.seg_es.access  = (saved_state[SMRAM_FIELD_AMD_K_ES_ACCESS] >> 8) & 0xff;
    cpu_state.seg_es.ar_high = (saved_state[SMRAM_FIELD_AMD_K_ES_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_es);

    /* CS */
    cpu_state.seg_cs.seg     = saved_state[SMRAM_FIELD_AMD_K_CS_SELECTOR];
    cpu_state.seg_cs.base    = saved_state[SMRAM_FIELD_AMD_K_CS_BASE];
    cpu_state.seg_cs.limit   = saved_state[SMRAM_FIELD_AMD_K_CS_LIMIT];
    cpu_state.seg_cs.access  = (saved_state[SMRAM_FIELD_AMD_K_CS_ACCESS] >> 8) & 0xff;
    cpu_state.seg_cs.ar_high = (saved_state[SMRAM_FIELD_AMD_K_CS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_cs);

    /* DS */
    cpu_state.seg_ds.seg     = saved_state[SMRAM_FIELD_AMD_K_DS_SELECTOR];
    cpu_state.seg_ds.base    = saved_state[SMRAM_FIELD_AMD_K_DS_BASE];
    cpu_state.seg_ds.limit   = saved_state[SMRAM_FIELD_AMD_K_DS_LIMIT];
    cpu_state.seg_ds.access  = (saved_state[SMRAM_FIELD_AMD_K_DS_ACCESS] >> 8) & 0xff;
    cpu_state.seg_ds.ar_high = (saved_state[SMRAM_FIELD_AMD_K_DS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_ds);

    /* SS */
    cpu_state.seg_ss.seg    = saved_state[SMRAM_FIELD_AMD_K_SS_SELECTOR];
    cpu_state.seg_ss.base   = saved_state[SMRAM_FIELD_AMD_K_SS_BASE];
    cpu_state.seg_ss.limit  = saved_state[SMRAM_FIELD_AMD_K_SS_LIMIT];
    cpu_state.seg_ss.access = (saved_state[SMRAM_FIELD_AMD_K_SS_ACCESS] >> 8) & 0xff;
    /* The actual CPL (DPL of CS) is overwritten with DPL of SS. */
    cpu_state.seg_cs.access  = (cpu_state.seg_cs.access & ~0x60) | (cpu_state.seg_ss.access & 0x60);
    cpu_state.seg_ss.ar_high = (saved_state[SMRAM_FIELD_AMD_K_SS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_ss);

    /* FS */
    cpu_state.seg_fs.seg     = saved_state[SMRAM_FIELD_AMD_K_FS_SELECTOR];
    cpu_state.seg_fs.base    = saved_state[SMRAM_FIELD_AMD_K_FS_BASE];
    cpu_state.seg_fs.limit   = saved_state[SMRAM_FIELD_AMD_K_FS_LIMIT];
    cpu_state.seg_fs.access  = (saved_state[SMRAM_FIELD_AMD_K_FS_ACCESS] >> 8) & 0xff;
    cpu_state.seg_fs.ar_high = (saved_state[SMRAM_FIELD_AMD_K_FS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_fs);

    /* GS */
    cpu_state.seg_gs.seg     = saved_state[SMRAM_FIELD_AMD_K_GS_SELECTOR];
    cpu_state.seg_gs.base    = saved_state[SMRAM_FIELD_AMD_K_GS_BASE];
    cpu_state.seg_gs.limit   = saved_state[SMRAM_FIELD_AMD_K_GS_LIMIT];
    cpu_state.seg_gs.access  = (saved_state[SMRAM_FIELD_AMD_K_GS_ACCESS] >> 8) & 0xff;
    cpu_state.seg_gs.ar_high = (saved_state[SMRAM_FIELD_AMD_K_GS_ACCESS] >> 16) & 0xff;
    smm_seg_load(&cpu_state.seg_gs);

    if (SMM_REVISION_ID & SMM_SMBASE_RELOCATION)
        smbase = saved_state[SMRAM_FIELD_AMD_K_SMBASE_OFFSET];
}

static void
smram_save_state_cyrix(uint32_t *saved_state, int in_hlt)
{
    saved_state[0] = dr[7];
    saved_state[1] = cpu_state.flags | (cpu_state.eflags << 16);
    saved_state[2] = cr0;
    saved_state[3] = cpu_state.oldpc;
    saved_state[4] = cpu_state.pc;
    saved_state[5] = CS | (CPL << 21);
    saved_state[6] = 0x00000000;
    saved_state[7] = 0x00010000;

    if (((opcode >= 0x6e) && (opcode <= 0x6f)) || ((opcode >= 0xe6) && (opcode <= 0xe7)) ||
        ((opcode >= 0xee) && (opcode <= 0xef))) {
        saved_state[6] |= 0x00000002;
        saved_state[7] = (opcode & 0x01) ? (cpu_state.op32 ? 0x000f0000 : 0x00030000) : 0x00010000;
    } else if (((opcode == 0xf2) || (opcode == 0xf3)) && (rep_op >= 0x6e) && (rep_op <= 0x6f)) {
        saved_state[6] |= 0x00000006;
        saved_state[7] = (rep_op & 0x01) ? (cpu_state.op32 ? 0x000f0000 : 0x00030000) : 0x00010000;
    } else if (((opcode == 0xf2) || (opcode == 0xf3)) && (rep_op >= 0x6e) && (rep_op <= 0x6f)) {
        saved_state[6] |= 0x00000004;
        saved_state[7] = (rep_op & 0x01) ? (cpu_state.op32 ? 0x000f0000 : 0x00030000) : 0x00010000;
    }

    if (is_smint) {
        saved_state[6] |= 0x00000008;
        is_smint = 0;
    }

    if (in_hlt)
        saved_state[6] |= 0x00000010;

    saved_state[7] |= io_port;
    saved_state[8] = io_val;

    if (saved_state[6] & 0x00000002)
        saved_state[9] = ESI;
    else
        saved_state[9] = EDI;
}

static void
smram_restore_state_cyrix(uint32_t *saved_state)
{
    dr[7]            = saved_state[0];
    cpu_state.flags  = saved_state[1] & 0xffff;
    cpu_state.eflags = saved_state[1] >> 16;
    cr0              = saved_state[2];
    cpu_state.pc     = saved_state[4];
    /* Restore CPL. */
    cpu_state.seg_cs.access = (cpu_state.seg_cs.access & ~0x9f) | (((saved_state[5] >> 21) & 0x03) << 5);

    if (saved_state[6] & 0x00000002)
        ESI = saved_state[9];
    else
        EDI = saved_state[9];
}

void
enter_smm(int in_hlt)
{
    uint32_t saved_state[SMM_SAVE_STATE_MAP_SIZE];
    uint32_t smram_state = smbase + 0x10000;

    /* If it's a CPU on which SMM is not supported, do nothing. */
    if (!is_am486 && !is_pentium && !is_k5 && !is_k6 && !is_p6 && !is_cxsmm)
        return;

    x386_common_log("enter_smm(): smbase = %08X\n", smbase);
    x386_common_log("CS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_cs.seg, cpu_state.seg_cs.base, cpu_state.seg_cs.limit, cpu_state.seg_cs.limit_low,
                    cpu_state.seg_cs.limit_high, cpu_state.seg_cs.access, cpu_state.seg_cs.ar_high);
    x386_common_log("DS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_ds.seg, cpu_state.seg_ds.base, cpu_state.seg_ds.limit, cpu_state.seg_ds.limit_low,
                    cpu_state.seg_ds.limit_high, cpu_state.seg_ds.access, cpu_state.seg_ds.ar_high);
    x386_common_log("ES : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_es.seg, cpu_state.seg_es.base, cpu_state.seg_es.limit, cpu_state.seg_es.limit_low,
                    cpu_state.seg_es.limit_high, cpu_state.seg_es.access, cpu_state.seg_es.ar_high);
    x386_common_log("FS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_fs.seg, cpu_state.seg_fs.base, cpu_state.seg_fs.limit, cpu_state.seg_fs.limit_low,
                    cpu_state.seg_fs.limit_high, cpu_state.seg_fs.access, cpu_state.seg_fs.ar_high);
    x386_common_log("GS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_gs.seg, cpu_state.seg_gs.base, cpu_state.seg_gs.limit, cpu_state.seg_gs.limit_low,
                    cpu_state.seg_gs.limit_high, cpu_state.seg_gs.access, cpu_state.seg_gs.ar_high);
    x386_common_log("SS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_ss.seg, cpu_state.seg_ss.base, cpu_state.seg_ss.limit, cpu_state.seg_ss.limit_low,
                    cpu_state.seg_ss.limit_high, cpu_state.seg_ss.access, cpu_state.seg_ss.ar_high);
    x386_common_log("TR : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    tr.seg, tr.base, tr.limit, tr.limit_low, tr.limit_high, tr.access, tr.ar_high);
    x386_common_log("LDT: seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    ldt.seg, ldt.base, ldt.limit, ldt.limit_low, ldt.limit_high, ldt.access, ldt.ar_high);
    x386_common_log("GDT: seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    gdt.seg, gdt.base, gdt.limit, gdt.limit_low, gdt.limit_high, gdt.access, gdt.ar_high);
    x386_common_log("IDT: seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    idt.seg, idt.base, idt.limit, idt.limit_low, idt.limit_high, idt.access, idt.ar_high);
    x386_common_log("CR0 = %08X, CR3 = %08X, CR4 = %08X, DR6 = %08X, DR7 = %08X\n", cr0, cr3, cr4, dr[6], dr[7]);
    x386_common_log("EIP = %08X, EFLAGS = %04X%04X\n", cpu_state.pc, cpu_state.eflags, cpu_state.flags);
    x386_common_log("EAX = %08X, EBX = %08X, ECX = %08X, EDX = %08X, ESI = %08X, EDI = %08X, ESP = %08X, EBP = %08X\n",
                    EAX, EBX, ECX, EDX, ESI, EDI, ESP, EBP);

    flags_rebuild();
    in_smm = 1;
    smram_backup_all();
    smram_recalc_all(0);

    if (is_cxsmm) {
        if (!(cyrix.smhr & SMHR_VALID))
            cyrix.smhr = (cyrix.arr[3].base + cyrix.arr[3].size) | SMHR_VALID;
        smram_state = cyrix.smhr & SMHR_ADDR_MASK;
    }

    memset(saved_state, 0x00, SMM_SAVE_STATE_MAP_SIZE * sizeof(uint32_t));

    if (is_cxsmm) /* Cx6x86 */
        smram_save_state_cyrix(saved_state, in_hlt);
    else if (is_pentium || is_am486) /* Am486 / 5x86 / Intel P5 (Pentium) */
        smram_save_state_p5(saved_state, in_hlt);
    else if (is_k5 || is_k6) /* AMD K5 and K6 */
        smram_save_state_amd_k(saved_state, in_hlt);
    else if (is_p6) /* Intel P6 (Pentium Pro, Pentium II, Celeron) */
        smram_save_state_p6(saved_state, in_hlt);

    cr0 &= ~0x8000000d;
    cpu_state.flags  = 2;
    cpu_state.eflags = 0;

    cr4 = 0;

    dr[7] = 0x400;

    if (is_cxsmm) {
        cpu_state.pc = 0x0000;
        cpl_override = 1;
        if (is486)
            cyrix_write_seg_descriptor(smram_state - 0x20, &cpu_state.seg_cs);
        else
            cyrix_write_seg_descriptor_2386(smram_state - 0x20, &cpu_state.seg_cs);
        cpl_override             = 0;
        cpu_state.seg_cs.seg     = (cyrix.arr[3].base >> 4);
        cpu_state.seg_cs.base    = cyrix.arr[3].base;
        cpu_state.seg_cs.limit   = 0xffffffff;
        cpu_state.seg_cs.access  = 0x93;
        cpu_state.seg_cs.ar_high = 0x80;
        cpu_state.seg_cs.checked = 1;

        smm_seg_load(&cpu_state.seg_cs);
    } else {
        cpu_state.pc             = 0x8000;
        cpu_state.seg_ds.seg     = 0x00000000;
        cpu_state.seg_ds.base    = 0x00000000;
        cpu_state.seg_ds.limit   = 0xffffffff;
        cpu_state.seg_ds.access  = 0x93;
        cpu_state.seg_ds.ar_high = 0x80;

        memcpy(&cpu_state.seg_es, &cpu_state.seg_ds, sizeof(x86seg));
        memcpy(&cpu_state.seg_ss, &cpu_state.seg_ds, sizeof(x86seg));
        memcpy(&cpu_state.seg_fs, &cpu_state.seg_ds, sizeof(x86seg));
        memcpy(&cpu_state.seg_gs, &cpu_state.seg_ds, sizeof(x86seg));

        if (is_p6)
            cpu_state.seg_cs.seg = (smbase >> 4);
        else
            cpu_state.seg_cs.seg = 0x3000;

        /* On Pentium, CS selector in SMM is always 3000, regardless of SMBASE. */
        cpu_state.seg_cs.base    = smbase;
        cpu_state.seg_cs.limit   = 0xffffffff;
        cpu_state.seg_cs.access  = 0x93;
        cpu_state.seg_cs.ar_high = 0x80;
        cpu_state.seg_cs.checked = 1;

        smm_seg_load(&cpu_state.seg_es);
        smm_seg_load(&cpu_state.seg_cs);
        smm_seg_load(&cpu_state.seg_ds);
        smm_seg_load(&cpu_state.seg_ss);
        smm_seg_load(&cpu_state.seg_fs);
        smm_seg_load(&cpu_state.seg_gs);
    }

    cpu_state.op32 = use32;

    cpl_override = 1;
    if (is_cxsmm) {
        writememl(0, smram_state - 0x04, saved_state[0]);
        writememl(0, smram_state - 0x08, saved_state[1]);
        writememl(0, smram_state - 0x0c, saved_state[2]);
        writememl(0, smram_state - 0x10, saved_state[3]);
        writememl(0, smram_state - 0x14, saved_state[4]);
        writememl(0, smram_state - 0x18, saved_state[5]);
        writememl(0, smram_state - 0x24, saved_state[6]);
        writememl(0, smram_state - 0x28, saved_state[7]);
        writememl(0, smram_state - 0x2c, saved_state[8]);
        writememl(0, smram_state - 0x30, saved_state[9]);
    } else {
        for (uint8_t n = 0; n < SMM_SAVE_STATE_MAP_SIZE; n++) {
            smram_state -= 4;
            writememl(0, smram_state, saved_state[n]);
        }
    }
    cpl_override = 0;

    nmi_mask = 0;

    if (smi_latched) {
        in_smm      = 2;
        smi_latched = 0;
    } else
        in_smm = 1;

    smm_in_hlt = in_hlt;

    if (unmask_a20_in_smm) {
        old_rammask = rammask;
        rammask     = cpu_16bitbus ? 0xFFFFFF : 0xFFFFFFFF;
        if (is6117)
            rammask |= 0x3000000;

        flushmmucache();
    }

    oldcpl = 0;

    cpu_cur_status &= ~(CPU_STATUS_PMODE | CPU_STATUS_V86);
    CPU_BLOCK_END();
}

void
enter_smm_check(int in_hlt)
{
    uint8_t ccr1_check;

    if (is_cx6x86)
        ccr1_check = ((ccr1 & (CCR1_USE_SMI | CCR1_SMAC | CCR1_SM3)) ==
                      (CCR1_USE_SMI | CCR1_SM3)) && (cyrix.arr[3].size > 0);
    else
        ccr1_check = ((ccr1 & (CCR1_USE_SMI | CCR1_SMAC)) ==
                      (CCR1_USE_SMI)) && (cyrix.arr[3].size > 0);

    if (smi_line) {
        if (!is_cxsmm || ccr1_check)  switch (in_smm) {
            default:
#ifdef ENABLE_386_COMMON_LOG
                fatal("SMI while in_smm = %i\n", in_smm);
                break;
#endif
            case 0:
#ifdef ENABLE_386_COMMON_LOG
                x386_common_log("SMI while not in SMM\n");
#endif
                enter_smm(in_hlt);
                break;
            case 1:
                /* Mark this so that we don't latch more than one SMI. */
#ifdef ENABLE_386_COMMON_LOG
                x386_common_log("SMI while in unlatched SMM\n");
#endif
                smi_latched = 1;
                break;
            case 2:
#ifdef ENABLE_386_COMMON_LOG
                x386_common_log("SMI while in latched SMM\n");
#endif
                break;
        }
#ifdef ENABLE_386_COMMON_LOG
        else {
            x386_common_log("SMI while in Cyrix disabled mode\n");
            x386_common_log("lol\n");
        }
#endif

        smi_line = 0;
    }
}

void
leave_smm(void)
{
    uint32_t saved_state[SMM_SAVE_STATE_MAP_SIZE];
    uint32_t smram_state = smbase + 0x10000;

    /* If it's a CPU on which SMM is not supported (or not implemented in 86Box), do nothing. */
    if (!is_am486 && !is_pentium && !is_k5 && !is_k6 && !is_p6 && !is_cxsmm)
        return;

    memset(saved_state, 0x00, SMM_SAVE_STATE_MAP_SIZE * sizeof(uint32_t));

    cpl_override = 1;
    if (is_cxsmm) {
        smram_state    = cyrix.smhr & SMHR_ADDR_MASK;
        saved_state[0] = readmeml(0, smram_state - 0x04);
        saved_state[1] = readmeml(0, smram_state - 0x08);
        saved_state[2] = readmeml(0, smram_state - 0x0c);
        saved_state[3] = readmeml(0, smram_state - 0x10);
        saved_state[4] = readmeml(0, smram_state - 0x14);
        saved_state[5] = readmeml(0, smram_state - 0x18);
        if (is486)
            cyrix_load_seg_descriptor(smram_state - 0x20, &cpu_state.seg_cs);
        else
            cyrix_load_seg_descriptor_2386(smram_state - 0x20, &cpu_state.seg_cs);
        saved_state[6] = readmeml(0, smram_state - 0x24);
        saved_state[7] = readmeml(0, smram_state - 0x28);
        saved_state[8] = readmeml(0, smram_state - 0x2c);
        saved_state[9] = readmeml(0, smram_state - 0x30);
    } else {
        for (uint8_t n = 0; n < SMM_SAVE_STATE_MAP_SIZE; n++) {
            smram_state -= 4;
            saved_state[n] = readmeml(0, smram_state);
            x386_common_log("Reading %08X from memory at %08X to array element %i\n", saved_state[n], smram_state, n);
        }
    }
    cpl_override = 0;

    if (unmask_a20_in_smm) {
        rammask = old_rammask;

        flushmmucache();
    }

    x386_common_log("New SMBASE: %08X (%08X)\n", saved_state[SMRAM_FIELD_P5_SMBASE_OFFSET], saved_state[66]);
    if (is_cxsmm) /* Cx6x86 */
        smram_restore_state_cyrix(saved_state);
    else if (is_pentium || is_am486) /* Am486 / 5x86 / Intel P5 (Pentium) */
        smram_restore_state_p5(saved_state);
    else if (is_k5 || is_k6) /* AMD K5 and K6 */
        smram_restore_state_amd_k(saved_state);
    else if (is_p6) /* Intel P6 (Pentium Pro, Pentium II, Celeron) */
        smram_restore_state_p6(saved_state);

    in_smm = 0;
    smram_recalc_all(1);

    cpu_386_flags_extract();
    cpu_cur_status &= ~(CPU_STATUS_PMODE | CPU_STATUS_V86);
    if (cr0 & 1) {
        cpu_cur_status |= CPU_STATUS_PMODE;
        if (cpu_state.eflags & VM_FLAG)
            cpu_cur_status |= CPU_STATUS_V86;
    }

    nmi_mask = 1;

    oldcpl = CPL;

    CPU_BLOCK_END();

    x386_common_log("CS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_cs.seg, cpu_state.seg_cs.base, cpu_state.seg_cs.limit, cpu_state.seg_cs.limit_low,
                    cpu_state.seg_cs.limit_high, cpu_state.seg_cs.access, cpu_state.seg_cs.ar_high);
    x386_common_log("DS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_ds.seg, cpu_state.seg_ds.base, cpu_state.seg_ds.limit, cpu_state.seg_ds.limit_low,
                    cpu_state.seg_ds.limit_high, cpu_state.seg_ds.access, cpu_state.seg_ds.ar_high);
    x386_common_log("ES : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_es.seg, cpu_state.seg_es.base, cpu_state.seg_es.limit, cpu_state.seg_es.limit_low,
                    cpu_state.seg_es.limit_high, cpu_state.seg_es.access, cpu_state.seg_es.ar_high);
    x386_common_log("FS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_fs.seg, cpu_state.seg_fs.base, cpu_state.seg_fs.limit, cpu_state.seg_fs.limit_low,
                    cpu_state.seg_fs.limit_high, cpu_state.seg_fs.access, cpu_state.seg_fs.ar_high);
    x386_common_log("GS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_gs.seg, cpu_state.seg_gs.base, cpu_state.seg_gs.limit, cpu_state.seg_gs.limit_low,
                    cpu_state.seg_gs.limit_high, cpu_state.seg_gs.access, cpu_state.seg_gs.ar_high);
    x386_common_log("SS : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    cpu_state.seg_ss.seg, cpu_state.seg_ss.base, cpu_state.seg_ss.limit, cpu_state.seg_ss.limit_low,
                    cpu_state.seg_ss.limit_high, cpu_state.seg_ss.access, cpu_state.seg_ss.ar_high);
    x386_common_log("TR : seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    tr.seg, tr.base, tr.limit, tr.limit_low, tr.limit_high, tr.access, tr.ar_high);
    x386_common_log("LDT: seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    ldt.seg, ldt.base, ldt.limit, ldt.limit_low, ldt.limit_high, ldt.access, ldt.ar_high);
    x386_common_log("GDT: seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    gdt.seg, gdt.base, gdt.limit, gdt.limit_low, gdt.limit_high, gdt.access, gdt.ar_high);
    x386_common_log("IDT: seg = %04X, base = %08X, limit = %08X, limit_low = %08X, limit_high = %08X, access = %02X, ar_high = %02X\n",
                    idt.seg, idt.base, idt.limit, idt.limit_low, idt.limit_high, idt.access, idt.ar_high);
    x386_common_log("CR0 = %08X, CR3 = %08X, CR4 = %08X, DR6 = %08X, DR7 = %08X\n", cr0, cr3, cr4, dr[6], dr[7]);
    x386_common_log("EIP = %08X, EFLAGS = %04X%04X\n", cpu_state.pc, cpu_state.eflags, cpu_state.flags);
    x386_common_log("EAX = %08X, EBX = %08X, ECX = %08X, EDX = %08X, ESI = %08X, EDI = %08X, ESP = %08X, EBP = %08X\n",
                    EAX, EBX, ECX, EDX, ESI, EDI, ESP, EBP);
    x386_common_log("leave_smm()\n");
}

void
x86_int(int num)
{
    uint32_t addr;
    static unsigned tp700_irq6_traces = 0;
    static unsigned tp700_irq2_traces = 0;
    static unsigned tp700_int1_traces = 0;
    static unsigned tp700_divide_traces = 0;

    if ((num == 0) && !strcmp(machine_get_internal_name(), "ibmtp700") &&
        (tp700_divide_traces++ < 64))
        pclog("TP700 DIVIDE ERROR: from=%04X oldpc=%04X pc=%04X "
              "AX=%04X BX=%04X DI=%04X DS=%04X operand=%02X "
              "target=%04X:%04X SS:SP=%04X:%04X\n",
              CS, (uint16_t) cpu_state.oldpc, (uint16_t) cpu_state.pc,
              AX, BX, DI, DS, readmemb(ds, (BX + DI + 1) & 0xffff),
              readmemw(0, 0x0002), readmemw(0, 0x0000), SS, SP);

    if ((num == 1) && !strcmp(machine_get_internal_name(), "ibmtp700") &&
        (tp700_int1_traces++ < 96))
        pclog("TP700 INT1: from=%04X oldpc=%04X pc=%04X FLAGS=%04X "
              "target=%04X:%04X SS:SP=%04X:%04X top=%04X %04X %04X\n",
              CS, (uint16_t) cpu_state.oldpc, (uint16_t) cpu_state.pc,
              cpu_state.flags, readmemw(0, 0x0006), readmemw(0, 0x0004),
              SS, SP, readmemw(ss, SP), readmemw(ss, (SP + 2) & 0xffff),
              readmemw(ss, (SP + 4) & 0xffff));

    if ((num == 0x0a) && !strcmp(machine_get_internal_name(), "ibmtp700")) {
        const unsigned trace_index = tp700_irq2_traces++;

        if ((trace_index < 64) || ((trace_index & 0xffff) == 0))
            pclog("TP700 IRQ2 CASCADE: index=%u from=%04X:%04X "
                  "master irr/isr/imr=%02X/%02X/%02X "
                  "slave irr/isr/imr=%02X/%02X/%02X pending=%u/%u\n",
                  trace_index, CS, (uint16_t) cpu_state.oldpc,
                  pic.irr, pic.isr, pic.imr,
                  pic2.irr, pic2.isr, pic2.imr,
                  pic.int_pending, pic2.int_pending);
    }

    if ((num == 0x0e) && !strcmp(machine_get_internal_name(), "ibmtp700") &&
        (tp700_irq6_traces++ < 64))
        pclog("TP700 IRQ6 ENTRY: from=%04X:%04X SS:SP=%04X:%04X "
              "FLAGS=%04X vector=%04X:%04X\n",
              CS, (uint16_t) cpu_state.oldpc, SS, (uint16_t) ESP,
              cpu_state.flags, readmemw(0, 0x003a), readmemw(0, 0x0038));

    if ((num == 2) && !strcmp(machine_get_internal_name(), "ibmtp700"))
        pclog("TP700 HW INT2: from %04X:%08X oldpc=%08X nmi=%d enable=%d mask=%02X "
              "state=%02X BDA0E=%04X BDA13=%04X\n",
              CS, cpu_state.pc, cpu_state.oldpc, nmi, nmi_enable, nmi_mask,
              ram[readmemw(0, 0x040e) * 16 + 1], readmemw(0, 0x040e),
              readmemw(0, 0x0413));

    flags_rebuild();
    cpu_state.pc = cpu_state.oldpc;

    if (msw & 1)
        cpu_use_exec ? pmodeint(num, 0) : pmodeint_2386(num, 0);
    else {
        addr = (num << 2) + idt.base;

        if ((num << 2UL) + 3UL > idt.limit) {
            if (idt.limit < 35) {
                cpu_state.abrt = 0;
                softresetx86();
                cpu_set_edx();
#ifdef ENABLE_386_COMMON_LOG
                x386_common_log("Triple fault in real mode - reset\n");
#endif
            } else
                x86_int(8);
        } else {
            if (stack32) {
                writememw(ss, ESP - 2, cpu_state.flags);
                writememw(ss, ESP - 4, CS);
                writememw(ss, ESP - 6, cpu_state.pc);
                ESP -= 6;
            } else {
                writememw(ss, ((SP - 2) & 0xFFFF), cpu_state.flags);
                writememw(ss, ((SP - 4) & 0xFFFF), CS);
                writememw(ss, ((SP - 6) & 0xFFFF), cpu_state.pc);
                SP -= 6;
            }

            cpu_state.flags &= ~I_FLAG;
            cpu_state.flags &= ~T_FLAG;
#ifndef USE_NEW_DYNAREC
            oxpc = cpu_state.pc;
#endif
            cpu_state.pc = readmemw(0, addr);
            cpu_use_exec ? loadcs(readmemw(0, addr + 2)) : loadcs_2386(readmemw(0, addr + 2));
        }
    }

    cycles -= 70;
    CPU_BLOCK_END();
}

static int      tp700_int21_pending;
static uint16_t tp700_int21_return_cs;
static uint16_t tp700_int21_return_ip;
static uint8_t  tp700_int21_pending_ah;
static uint8_t  tp700_int21_pending_al;
static char     tp700_int21_pending_path[129];

/*
 * The 9552 ESDI BIOS switches between the native disk, the OS-visible disk,
 * and the hidden 3 MiB System Partition.  These values describe controller
 * state, not DOS state, so a hard reset must return them to power-on values.
 * Keeping them as function-local statics made a Reference-Diskette-requested
 * POST briefly inherit the old System Partition window; depending on the ROM
 * timing, an early IML read could then address the wrong physical sector and
 * report I9990303.
 */
static uint32_t tp700_visible_sectors;
static int      tp700_service_window;
static int      tp700_reference_floppy_boot;

void
tp700_int13_reset_state(void)
{
    tp700_visible_sectors       = 0;
    tp700_service_window        = 0;
    tp700_reference_floppy_boot = 0;
    pclog("TP700 INT13: ESDI/System Partition window reset to power-on state\n");
}

void
tp700_int21_trace_begin(uint16_t return_cs, uint16_t return_ip, uint8_t ah, uint8_t al,
                        const char *path)
{
    tp700_int21_pending    = 1;
    tp700_int21_return_cs  = return_cs;
    tp700_int21_return_ip  = return_ip;
    tp700_int21_pending_ah = ah;
    tp700_int21_pending_al = al;
    strncpy(tp700_int21_pending_path, path, sizeof(tp700_int21_pending_path) - 1);
    tp700_int21_pending_path[sizeof(tp700_int21_pending_path) - 1] = 0;
}

void
tp700_int21_trace_return(uint16_t return_cs, uint16_t return_ip, uint16_t ax, uint16_t flags)
{
    if (!tp700_int21_pending || (return_cs != tp700_int21_return_cs) ||
        (return_ip != tp700_int21_return_ip))
        return;

    pclog("TP700 INT21 RESULT: AH=%02X AL=%02X path=\"%s\" "
          "carry=%u AX=%04X\n",
          tp700_int21_pending_ah, tp700_int21_pending_al,
          tp700_int21_pending_path, !!(flags & C_FLAG), ax);
    tp700_int21_pending = 0;
}

void
x86_int_sw(int num)
{
    uint32_t addr;
    static unsigned tp700_int13_traces = 0;
    static unsigned tp700_int21_file_traces = 0;
    static unsigned tp700_sw_int0a_traces = 0;
    static unsigned tp700_sw_int1c_traces = 0;
    static int      tp700_int13_caller_dumped = 0;

    /*
     * IBM's ESDI system-partition extension can reduce the drive's reported
     * last RBA while leaving the physical medium intact.  Zero means that no
     * reduced boundary has been selected yet, so the complete image is
     * visible.  The Reference Diskette restores the native boundary during
     * POST and later hides its 3 MiB service area before booting the OS.
     */
    /*
     * Trace the DOS file requests made by the Reference Diskette while the
     * ThinkPad-specific configuration path is being validated.  This is kept
     * deliberately narrow: only calls which identify a drive or pathname are
     * recorded, so normal console I/O does not drown out the failing request.
     */
    if ((num == 0x21) && !strcmp(machine_get_internal_name(), "ibmtp700") &&
        (tp700_int21_file_traces < 4096)) {
        const int has_path = (AH == 0x3c) || (AH == 0x3d) || (AH == 0x41) ||
                             (AH == 0x43) || (AH == 0x4e) || (AH == 0x56) ||
                             (AH == 0x5b) || (AH == 0x6c);

        if (has_path || (AH == 0x0e) || (AH == 0x19) || (AH == 0x36) ||
            (AH == 0x39) || (AH == 0x3a) || (AH == 0x3b)) {
            char path[129];
            unsigned path_length = 0;

            const uint16_t path_offset = (AH == 0x6c) ? SI : DX;

            if (has_path || (AH == 0x39) || (AH == 0x3a) || (AH == 0x3b)) {
                while (path_length < sizeof(path) - 1) {
                    const uint8_t byte = readmembl(ds + ((path_offset + path_length) & 0xffff));

                    if (!byte)
                        break;
                    path[path_length++] = ((byte >= 0x20) && (byte < 0x7f)) ? byte : '.';
                }
            }
            path[path_length] = 0;
            tp700_int21_file_traces++;
            pclog("TP700 INT21 FILE: index=%u from=%04X:%04X AH=%02X "
                  "AL=%02X BX=%04X CX=%04X DX=%04X DS=%04X path=\"%s\" "
                  "target=%04X:%04X\n",
                  tp700_int21_file_traces, CS,
                  (uint16_t) (cpu_state.pc - 2), AH, AL, BX, CX, DX, DS, path,
                  readmemw(0, 0x0086), readmemw(0, 0x0084));
            tp700_int21_trace_begin(CS, (uint16_t) cpu_state.pc, AH, AL, path);
        }
    }

    if ((num == 0x1c) && !strcmp(machine_get_internal_name(), "ibmtp700") &&
        (tp700_sw_int1c_traces++ < 64))
        pclog("TP700 SW INT1C: from=%04X:%04X target=%04X:%04X "
              "bytes=%02X %02X %02X %02X %02X "
              "SS:SP=%04X:%04X master=%02X/%02X/%02X\n",
              CS, (uint16_t) (cpu_state.pc - 2),
              readmemw(0, 0x0072), readmemw(0, 0x0070),
              readmemb(0, 0xfff53), readmemb(0, 0xfff54),
              readmemb(0, 0xfff55), readmemb(0, 0xfff56),
              readmemb(0, 0xfff57),
              SS, SP, pic.irr, pic.isr, pic.imr);

    if ((num == 0x0a) && !strcmp(machine_get_internal_name(), "ibmtp700")) {
        const unsigned trace_index = tp700_sw_int0a_traces++;

        if (trace_index < 64)
            pclog("TP700 SW INT0A: index=%u from=%04X:%04X "
                  "AX=%04X BX=%04X CX=%04X DX=%04X SS:SP=%04X:%04X\n",
                  trace_index, CS, (uint16_t) (cpu_state.pc - 2),
                  AX, BX, CX, DX, SS, SP);
    }

    if ((num == 0x13) && !strcmp(machine_get_internal_name(), "ibmtp700")) {
        /*
         * Once the Reference Diskette's own boot code starts issuing floppy
         * requests, its fixed disk C: is the logical System Partition.  The
         * ESDI controller keeps that alternate window as controller state;
         * it is not re-selected by every DOS program.  Model that state here
         * before DOS performs its first fixed-disk probe.
         */
        if (!(DL & 0x80) && (CS != 0xf000) && !tp700_hdd_os_active &&
            (tp700_hdd_boot_pending != 2)) {
            tp700_reference_floppy_boot = 1;
            tp700_post_memory_display_finish();
        }

        if ((DL & 0x80) && (CS != 0xf000) &&
            tp700_reference_floppy_boot && !tp700_hdd_os_active &&
            (tp700_hdd_boot_pending != 2) && !tp700_service_window) {
            tp700_service_window = 1;
            pclog("TP700 INT13: Reference-Diskette DOS selected System "
                  "Partition window\n");
        }

        /*
         * A Reference-Diskette "Start operating system" request performs a
         * complete hard POST first.  At the ROM's normal A: boot-sector read,
         * make the one-shot hand-off to the hard-disk MBR instead.  The
         * Reference Diskette stays mounted for the next cold start.
         */
        if ((tp700_hdd_boot_pending == 1) && (CS == 0xf000) && (AH == 0x02) &&
            (AL == 0x01) && (ES == 0x0000) && (BX == 0x7c00) &&
            (CX == 0x0001) && (DX == 0x0000)) {
            /*
             * Let the ROM finish its own bootstrap path, but substitute the
             * fixed disk for its pending A: boot-sector request.  Returning
             * through the ROM is important: it leaves the post-POST IVT/BDA
             * and stack intact, and it carries DL=80h into the MBR exactly as
             * a conventional hard-disk boot does.
             */
            DL = 0x80;
            tp700_service_window       = 0;
            tp700_reference_floppy_boot = 0;
            /* The ROM has already selected the OS-visible ESDI boundary. */
            {
                const uint32_t ebda = (uint32_t) readmemw(0, 0x040e) << 4;

                if (ebda && (ebda + 0x00be < mem_size * 1024))
                    writemembl(ebda + 0x00be, 0x00);
            }
            /* Stage 2 reloads the MBR at the ROM's final 0000:7C00 jump. */
            tp700_hdd_boot_pending = 2;
            pclog("TP700 BOOT HANDOFF: substituting HDD MBR for ROM A: read\n");
        }

        const unsigned trace_index = tp700_int13_traces++;

        if ((trace_index < 96) || ((trace_index & 0x3f) == 0) || (DL & 0x80))
            pclog("TP700 INT13 CALL: index=%u from=%04X:%04X phys=%08X AX=%04X "
                  "BX=%04X CX=%04X DX=%04X ES=%04X buffer=%08X "
                  "SS:SP=%04X:%04X FLAGS=%04X vector=%04X:%04X\n",
                  trace_index, CS, (uint16_t) (cpu_state.pc - 2),
                  cs + ((cpu_state.pc - 2) & 0xffff), AX, BX,
                  CX, DX, ES, es + BX, SS, SP, cpu_state.flags,
                  readmemw(0, 0x004e), readmemw(0, 0x004c));

        /*
         * The 9552 ROM talks to its planar ESDI hardware through a small
         * serial-storage front end which the desktop Model 50 base does not
         * provide.  Its Reference Diskette consequently sees the DF9F POS
         * adapter, but the BIOS leaves the fixed-disk count at zero and its
         * INT 13h path cannot transfer sectors.  Register the configured
         * ESDI image once DOS starts using the BIOS, and service the standard
         * fixed-disk calls here.  Floppy calls still follow the real 9552 ROM.
         */
        if ((CS != 0xf000) && hdd_is_valid(0))
            ram[0x475] = 1;

        if ((DL & 0x80) && hdd_is_valid(0)) {
            const uint32_t cylinders = hdd[0].tracks;
            const uint32_t heads     = hdd[0].hpc;
            const uint32_t sectors   = hdd[0].spt;
            const uint32_t total     = cylinders * heads * sectors;
            const uint32_t visible_total =
                (tp700_visible_sectors && (tp700_visible_sectors <= total)) ?
                tp700_visible_sectors : total;
            const uint32_t service_sectors = 0x1800;
            const uint32_t access_base =
                tp700_service_window && (total >= service_sectors) ?
                total - service_sectors : 0;
            const uint32_t accessible_total =
                tp700_service_window && (total >= service_sectors) ?
                service_sectors : visible_total;
            const uint32_t cylinder  = CH | ((CL & 0xc0) << 2);
            const uint32_t sector    = CL & 0x3f;
            const uint32_t count     = AL ? AL : 256;
            uint32_t       lba       = ((cylinder * heads) + DH) * sectors +
                                     (sector ? sector - 1 : 0);
            const uint32_t buffer    = es + BX;
            uint8_t       *transfer  = NULL;
            int            result    = 0;
            int            handled   = 1;

            flags_rebuild();
            switch (AH) {
                case 0x00: /* Reset disk system. */
                case 0x09: /* Initialize drive parameters. */
                case 0x0c: /* Seek. */
                case 0x0d: /* Alternate disk reset. */
                case 0x10: /* Check drive ready. */
                case 0x11: /* Recalibrate. */
                case 0x14: /* Controller diagnostic. */
                case 0x16: /* Disk change status. */
                case 0x17: /* Set disk type. */
                case 0x19: /* Park heads. */
                    AH = 0;
                    break;

                case 0x01: /* Return last operation status. */
                    AH = 0;
                    break;

                case 0x1f:
                    /*
                     * Private IBM ESDI system-partition extension used by the
                     * ThinkPad 700 ROM and Reference Diskette:
                     *
                     *   AL=03 returns the native last RBA in CX:DX.
                     *   AL=01 converts the absolute RBA in BX:CX to CHS.
                     *   AL=00 selects the last OS-visible RBA in BX:CX.
                     *   AL=02 maps CHS zero to the 3 MiB service area.
                     *
                     * The ROM subtracts 17FFh from the returned last RBA,
                     * producing the first sector of its 1800h-sector (3 MiB)
                     * service area, and then passes start-1 to AL=00.  Thus
                     * the service area remains physically present but is not
                     * exposed through normal INT 13h calls after POST.
                     */
                    if (AL == 0x00) {
                        const uint32_t last_rba = ((uint32_t) BX << 16) | CX;

                        if (last_rba >= total) {
                            AH     = 0x04;
                            result = -1;
                            break;
                        }
                        tp700_visible_sectors = last_rba + 1;
                        tp700_service_window = 0;
                        AH = 0;
                        pclog("TP700 INT13 1F00: visible last RBA=%u "
                              "sectors=%u/%u\n",
                              last_rba, tp700_visible_sectors, total);
                        break;
                    }
                    if (AL == 0x01) {
                        const uint32_t rba = ((uint32_t) BX << 16) | CX;
                        const int rba_in_window = !tp700_service_window ||
                                                  ((rba >= access_base) &&
                                                   (rba < total));
                        const uint32_t window_rba =
                            tp700_service_window && rba_in_window ?
                            rba - access_base : rba;
                        const uint32_t chs_cylinder =
                            window_rba / (heads * sectors);
                        const uint32_t chs_remainder =
                            window_rba % (heads * sectors);
                        const uint32_t chs_head = chs_remainder / sectors;
                        const uint32_t chs_sector = (chs_remainder % sectors) + 1;

                        if (!rba_in_window || (rba >= total) ||
                            (window_rba >= accessible_total) ||
                            (chs_cylinder >= 1024)) {
                            if (!tp700_int13_caller_dumped) {
                                const uint32_t caller = cs + ((cpu_state.pc - 2) & 0xffff);
                                const uint16_t return_ip = readmemw(ss, SP);
                                const uint16_t return_cs = readmemw(ss, (SP + 2) & 0xffff);
                                const uint32_t return_base =
                                    (return_cs == CS) ? cs : ((uint32_t) return_cs << 4);
                                const uint32_t return_linear = return_base + return_ip;

                                tp700_int13_caller_dumped = 1;
                                pclog("TP700 INT13 1F01 caller dump: CS=%04X base=%08X "
                                      "IP=%04X linear=%08X return=%04X:%04X "
                                      "return-linear=%08X stack=%04X/%04X/%04X/%04X\n",
                                      CS, cs, (uint16_t) (cpu_state.pc - 2), caller,
                                      return_cs, return_ip, return_linear,
                                      readmemw(ss, SP), readmemw(ss, (SP + 2) & 0xffff),
                                      readmemw(ss, (SP + 4) & 0xffff),
                                      readmemw(ss, (SP + 6) & 0xffff));
                                for (uint32_t row = caller - 128; row < caller + 128; row += 16) {
                                    pclog("TP700 CALLER %08X:", row);
                                    for (uint32_t column = 0; column < 16; column++)
                                        pclog(" %02X", readmembl(row + column));
                                    pclog("\n");
                                }
                                for (uint32_t row = return_linear - 256;
                                     row < return_linear + 256; row += 16) {
                                    pclog("TP700 RETURN %08X:", row);
                                    for (uint32_t column = 0; column < 16; column++)
                                        pclog(" %02X", readmembl(row + column));
                                    pclog("\n");
                                }
                            }
                            AH     = 0x04;
                            result = -1;
                            break;
                        }
                        CH = chs_cylinder & 0xff;
                        CL = chs_sector | ((chs_cylinder >> 2) & 0xc0);
                        DH = chs_head;
                        AH = 0;
                        pclog("TP700 INT13 1F01: RBA=%u window-RBA=%u -> "
                              "CHS=%u/%u/%u\n", rba, window_rba,
                              chs_cylinder, chs_head, chs_sector);
                        break;
                    }
                    if (AL == 0x02) {
                        if (total < service_sectors) {
                            AH     = 0x04;
                            result = -1;
                            break;
                        }
                        /*
                         * The maintenance programs switch the ESDI BIOS to a
                         * separate logical drive window before formatting and
                         * copying the System Partition.  Their following CHS
                         * 0/0/1 request therefore means the first sector of
                         * the reserved area, not physical sector zero.
                         */
                        tp700_service_window = 1;
                        AH = 0;
                        pclog("TP700 INT13 1F02: service window base=%u "
                              "sectors=%u\n", total - service_sectors,
                              service_sectors);
                    } else if (AL == 0x03) {
                        const uint32_t last_rba = total - 1;

                        CX = last_rba >> 16;
                        DX = last_rba & 0xffff;
                        AH = 0;
                        pclog("TP700 INT13 1F03: native last RBA=%u "
                              "CX:DX=%04X:%04X\n",
                              last_rba, CX, DX);
                    } else {
                        AH     = 0x01;
                        result = -1;
                    }
                    break;

                case 0x02: /* Read sectors. */
                case 0x03: /* Write sectors. */
                case 0x04: /* Verify sectors. */
                case 0x0a: /* Read long (data portion). */
                case 0x0b: /* Write long (data portion). */
                    if (!sector || (DH >= heads) ||
                        (cylinder >= cylinders) ||
                        (lba + count > accessible_total)) {
                        result = -1;
                        AH     = 0x04;
                        break;
                    }
                    if (AH != 0x04) {
                        transfer = malloc(count * 512);
                        if (!transfer) {
                            result = -1;
                            AH     = 0x20;
                            break;
                        }
                        if ((AH == 0x03) || (AH == 0x0b)) {
                            for (uint32_t i = 0; i < count * 512; i++)
                                transfer[i] = readmembl(buffer + i);
                            result = hdd_image_write(0, access_base + lba,
                                                     count, transfer);
                        } else {
                            result = hdd_image_read(0, access_base + lba,
                                                    count, transfer);
                            if (!result)
                                for (uint32_t i = 0; i < count * 512; i++)
                                    writemembl(buffer + i, transfer[i]);
                        }
                        free(transfer);
                    }
                    if (!result) {
                        AH = 0;
                        AL = count & 0xff;
                    } else if (!AH)
                        AH = 0x20;
                    break;

                case 0x05: /* Format track. */
                case 0x06: /* Format bad track. */
                case 0x07: /* Format drive. */
                    if ((DH >= heads) || (cylinder >= cylinders) ||
                        ((((cylinder * heads) + DH) * sectors) + sectors >
                         accessible_total)) {
                        result = -1;
                        AH     = 0x04;
                    } else {
                        const uint32_t track_lba =
                            ((cylinder * heads) + DH) * sectors;
                        result = hdd_image_zero(0, access_base + track_lba,
                                                sectors);
                        AH     = result ? 0x20 : 0;
                    }
                    break;

                case 0x08: { /* Get drive parameters. */
                    const uint32_t visible_cylinders =
                        (accessible_total + (heads * sectors) - 1) /
                        (heads * sectors);
                    const uint32_t last_cylinder = visible_cylinders - 1;
                    CH = last_cylinder & 0xff;
                    CL = sectors | ((last_cylinder >> 2) & 0xc0);
                    DH = heads - 1;
                    DL = 1;
                    AH = 0;
                    break;
                }

                case 0x15: /* Get drive type and sector count. */
                    AH = 0x03;
                    CX = accessible_total >> 16;
                    DX = accessible_total & 0xffff;
                    break;

                case 0x1a: /* Format ESDI unit. */
                    result = hdd_image_zero(0, access_base, accessible_total);
                    AH     = result ? 0x20 : 0;
                    break;

                default:
                    handled = 0;
                    break;
            }

            if (handled) {
                if (result)
                    cpu_state.flags |= C_FLAG;
                else
                    cpu_state.flags &= ~C_FLAG;
                flags_extract();
                cycles -= timing_int;
                pclog("TP700 INT13 ESDI: AH=%02X lba=%u physical=%u count=%u result=%d "
                      "AX=%04X CX=%04X DX=%04X FLAGS=%04X\n",
                      AH, lba, access_base + lba, count, result, AX, CX, DX,
                      cpu_state.flags);
                /* A dynarec block may run across the ROM's F29DDh/7C00h
                   boundary, so the fetch-loop hand-off is not guaranteed to
                   observe stage 2.  Once the substituted boot-sector read
                   has succeeded, enter the already-loaded HDD MBR here. */
                if (!result && (tp700_hdd_boot_pending == 2)) {
                    pclog("TP700 BOOT HANDOFF: entering HDD MBR after "
                          "substituted boot read\n");
                    tp700_boot_hdd_post();
                }
                CPU_BLOCK_END();
                return;
            }
        }
    }

    if (0 && (num == 0x15) && !strcmp(machine_get_internal_name(), "ibmtp700") &&
        ((AX == 0x9001) || (AX == 0x9101) ||
         ((AX & 0xff00) == 0x2100) ||
         ((AX & 0xff00) == 0xc200) || ((AX & 0xff00) == 0xd200)))
        pclog("TP700 INT15 CALL: AX=%04X BX=%04X CX=%04X DX=%04X "
              "ES:DI=%04X:%04X DS:SI=%04X:%04X from=%04X:%04X "
              "SS:SP=%04X:%04X FLAGS=%04X vector=%04X:%04X msw=%04X "
              "use32=%03X stack32=%d stack=%04X %04X %04X %04X\n",
              AX, BX, CX, DX, ES, DI, DS, SI,
              CS, (uint16_t) (cpu_state.pc - 2), SS, (uint16_t) ESP,
              cpu_state.flags, readmemw(0, 0x0056), readmemw(0, 0x0054),
              msw, use32, stack32,
              readmemw(ss, SP), readmemw(ss, (SP + 2) & 0xffff),
              readmemw(ss, (SP + 4) & 0xffff),
              readmemw(ss, (SP + 6) & 0xffff));

    if ((num == 2) && !strcmp(machine_get_internal_name(), "ibmtp700"))
        pclog("TP700 SW INT2: from %04X:%08X nmi=%d enable=%d mask=%02X\n",
              CS, cpu_state.pc, nmi, nmi_enable, nmi_mask);

    flags_rebuild();
    cycles -= timing_int;

    if (msw & 1)
        cpu_use_exec ? pmodeint(num, 1) : pmodeint_2386(num, 1);
    else {
        addr = (num << 2) + idt.base;

        if ((num << 2UL) + 3UL > idt.limit)
            x86_int(0x0d);
        else {
            if (stack32) {
                writememw(ss, ESP - 2, cpu_state.flags);
                writememw(ss, ESP - 4, CS);
                writememw(ss, ESP - 6, cpu_state.pc);
                ESP -= 6;
            } else {
                writememw(ss, ((SP - 2) & 0xFFFF), cpu_state.flags);
                writememw(ss, ((SP - 4) & 0xFFFF), CS);
                writememw(ss, ((SP - 6) & 0xFFFF), cpu_state.pc);
                SP -= 6;
            }

            cpu_state.flags &= ~I_FLAG;
            cpu_state.flags &= ~T_FLAG;
#ifndef USE_NEW_DYNAREC
            oxpc = cpu_state.pc;
#endif
            cpu_state.pc = readmemw(0, addr);
            cpu_use_exec ? loadcs(readmemw(0, addr + 2)) : loadcs_2386(readmemw(0, addr + 2));
            cycles -= timing_int_rm;
        }
    }

#ifdef USE_DEBUG_REGS_486
    trap &= ~1;
#else
    if (cpu_use_exec)
        trap = 0;
    else
        trap &= ~1;
#endif
    CPU_BLOCK_END();
}

int
x86_int_sw_rm(int num)
{
    uint32_t addr;
    uint16_t new_pc;
    uint16_t new_cs;

    flags_rebuild();
    cycles -= timing_int;

    addr   = num << 2;
    new_pc = readmemw(0, addr);
    new_cs = readmemw(0, addr + 2);

    if (cpu_state.abrt)
        return 1;

    writememw(ss, ((SP - 2) & 0xFFFF), cpu_state.flags);

    if (cpu_state.abrt)
        return 1;

    writememw(ss, ((SP - 4) & 0xFFFF), CS);
    writememw(ss, ((SP - 6) & 0xFFFF), cpu_state.pc);

    if (cpu_state.abrt)
        return 1;

    SP -= 6;

    cpu_state.eflags &= ~VIF_FLAG;
    cpu_state.flags &= ~T_FLAG;
    cpu_state.pc = new_pc;
    cpu_use_exec ? loadcs(new_cs) : loadcs_2386(new_cs);
#ifndef USE_NEW_DYNAREC
    oxpc = cpu_state.pc;
#endif

    cycles -= timing_int_rm;
#ifdef USE_DEBUG_REGS_486
    trap &= ~1;
#else
    if (cpu_use_exec)
        trap = 0;
    else
        trap &= ~1;
#endif
    CPU_BLOCK_END();

    return 0;
}

void
x86illegal(void)
{
    x86_int(6);
}

int
checkio(uint32_t port, int mask)
{
    uint32_t t;

    if (!(tr.access & 0x08)) {
        if ((CPL) > (IOPL))
            return 1;

        return 0;
    }

    cpl_override = 1;
    t            = readmemw(tr.base, 0x66);

    if (UNLIKELY(cpu_state.abrt)) {
        cpl_override = 0;
        return 0;
    }

    t += (port >> 3UL);
    mask <<= (port & 7);
    if (UNLIKELY(mask & 0xff00)) {
        if (LIKELY(t < tr.limit))
            mask &= readmemwl(tr.base + t);
    } else {
        if (LIKELY(t <= tr.limit))
            mask &= readmembl(tr.base + t);
    }
    cpl_override = 0;
    return mask;
}

#ifdef OLD_DIVEXCP
#    define divexcp()                                                                       \
        {                                                                                   \
            x386_common_log("Divide exception at %04X(%06X):%04X\n", CS, cs, cpu_state.pc); \
            x86_int(0);                                                                     \
        }
#else
#    define divexcp()       \
        {                   \
            x86de(NULL, 0); \
        }
#endif

int
divl(uint32_t val)
{
    uint64_t num;
    uint64_t quo;
    uint32_t rem;
    uint32_t quo32;

    if (val == 0) {
        divexcp();
        return 1;
    }

    num   = (((uint64_t) EDX) << 32) | EAX;
    quo   = num / val;
    rem   = num % val;
    quo32 = (uint32_t) (quo & 0xFFFFFFFF);

    if (quo != (uint64_t) quo32) {
        divexcp();
        return 1;
    }

    EDX = rem;
    EAX = quo32;

    return 0;
}

int
idivl(int32_t val)
{
    int64_t num;
    int64_t quo;
    int32_t rem;
    int32_t quo32;

    if (val == 0) {
        divexcp();
        return 1;
    }

    num   = (((uint64_t) EDX) << 32) | EAX;
    quo   = num / val;
    rem   = num % val;
    quo32 = (int32_t) (quo & 0xFFFFFFFF);

    if (quo != (int64_t) quo32) {
        divexcp();
        return 1;
    }

    EDX = rem;
    EAX = quo32;

    return 0;
}

void
cpu_386_flags_extract(void)
{
    flags_extract();
}

void
cpu_386_flags_rebuild(void)
{
    flags_rebuild();
}

extern uint64_t mmutranslate_noabrt_2386(uint32_t addr, int rw);
int
cpu_386_check_instruction_fault(void)
{
    int i = 0;
    int fault = 0;
    /* Report no fault if RF is set. */
    if (cpu_state.eflags & RF_FLAG)
        return 0;

    /* Make sure breakpoints are enabled. */
    if (!(dr[7] & 0xFF))
        return 0;

    for (i = 0; i < 4; i++) {
        int breakpoint_enabled = !!(dr[7] & (0x3 << (2 * i))) && !(dr[7] & (0x30000 << (4 * i)));
        uint32_t translated_addr = 0xffffffff;
        if (!breakpoint_enabled)
            continue;

        translated_addr = dr[i];

        if ((cs + cpu_state.pc) == (uint32_t)translated_addr) {
            dr[6] |= (1 << i);
            fault = 1;
        }
    }

    return fault;
}

int
sysenter(UNUSED(uint32_t fetchdat))
{
#ifdef ENABLE_386_COMMON_LOG
    x386_common_log("SYSENTER called\n");
#endif

    if (!(msw & 1)) {
#ifdef ENABLE_386_COMMON_LOG
        x386_common_log("SYSENTER: CPU not in protected mode");
#endif
        x86gpf("SYSENTER: CPU not in protected mode", 0);
        return cpu_state.abrt;
    }

    if (!(msr.sysenter_cs & 0xFFF8)) {
#ifdef ENABLE_386_COMMON_LOG
        x386_common_log("SYSENTER: CS MSR is zero");
#endif
        x86gpf("SYSENTER: CS MSR is zero", 0);
        return cpu_state.abrt;
    }

#ifdef ENABLE_386_COMMON_LOG
    x386_common_log("SYSENTER started:\n");
    x386_common_log("    CS %04X/%i: b=%08X l=%08X (%08X-%08X) a=%02X%02X; EIP=%08X\n", cpu_state.seg_cs.seg, !!cpu_state.seg_cs.checked, cpu_state.seg_cs.base, cpu_state.seg_cs.limit, cpu_state.seg_cs.limit_low, cpu_state.seg_cs.limit_high, cpu_state.seg_cs.ar_high, cpu_state.seg_cs.access, cpu_state.pc);
    x386_common_log("    SS %04X/%i: b=%08X l=%08X (%08X-%08X) a=%02X%02X; ESP=%08X\n", cpu_state.seg_ss.seg, !!cpu_state.seg_ss.checked, cpu_state.seg_ss.base, cpu_state.seg_ss.limit, cpu_state.seg_ss.limit_low, cpu_state.seg_ss.limit_high, cpu_state.seg_ss.ar_high, cpu_state.seg_ss.access, ESP);
    x386_common_log("    Misc.  : MSR (CS/ESP/EIP)=%04X/%08X/%08X pccache=%08X/%08X\n", msr.sysenter_cs, msr.sysenter_esp, msr.sysenter_eip, pccache, pccache2);
    x386_common_log("             EFLAGS=%04X%04X/%i 32=%i/%i ECX=%08X EDX=%08X abrt=%02X\n", cpu_state.eflags, cpu_state.flags, !!trap, !!use32, !!stack32, ECX, EDX, cpu_state.abrt);
#endif

    /* Set VM, RF, and IF to 0. */
    cpu_state.eflags &= ~(RF_FLAG | VM_FLAG);
    cpu_state.flags &= ~I_FLAG;

#ifndef USE_NEW_DYNAREC
    oldcs = CS;
#endif
    cpu_state.oldpc = cpu_state.pc;
    ESP             = msr.sysenter_esp;
    cpu_state.pc    = msr.sysenter_eip;

    cpu_state.seg_cs.seg        = (msr.sysenter_cs & 0xfffc);
    cpu_state.seg_cs.base       = 0;
    cpu_state.seg_cs.limit_low  = 0;
    cpu_state.seg_cs.limit      = 0xffffffff;
    cpu_state.seg_cs.limit_high = 0xffffffff;
    cpu_state.seg_cs.access     = 0x9b;
    cpu_state.seg_cs.ar_high    = 0xcf;
    cpu_state.seg_cs.checked    = 1;
    oldcpl                      = 0;

    cpu_state.seg_ss.seg        = ((msr.sysenter_cs + 8) & 0xfffc);
    cpu_state.seg_ss.base       = 0;
    cpu_state.seg_ss.limit_low  = 0;
    cpu_state.seg_ss.limit      = 0xffffffff;
    cpu_state.seg_ss.limit_high = 0xffffffff;
    cpu_state.seg_ss.access     = 0x93;
    cpu_state.seg_ss.ar_high    = 0xcf;
    cpu_state.seg_ss.checked    = 1;
#ifdef USE_DYNAREC
    codegen_flat_ss = 0;
#endif

    cpu_cur_status &= ~(CPU_STATUS_NOTFLATSS | CPU_STATUS_V86);
    cpu_cur_status |= (CPU_STATUS_USE32 | CPU_STATUS_STACK32 /* | CPU_STATUS_PMODE*/);
    set_use32(1);
    set_stack32(1);

    in_sys = 1;

#ifdef ENABLE_386_COMMON_LOG
    x386_common_log("SYSENTER completed:\n");
    x386_common_log("    CS %04X/%i: b=%08X l=%08X (%08X-%08X) a=%02X%02X; EIP=%08X\n", cpu_state.seg_cs.seg, !!cpu_state.seg_cs.checked, cpu_state.seg_cs.base, cpu_state.seg_cs.limit, cpu_state.seg_cs.limit_low, cpu_state.seg_cs.limit_high, cpu_state.seg_cs.ar_high, cpu_state.seg_cs.access, cpu_state.pc);
    x386_common_log("    SS %04X/%i: b=%08X l=%08X (%08X-%08X) a=%02X%02X; ESP=%08X\n", cpu_state.seg_ss.seg, !!cpu_state.seg_ss.checked, cpu_state.seg_ss.base, cpu_state.seg_ss.limit, cpu_state.seg_ss.limit_low, cpu_state.seg_ss.limit_high, cpu_state.seg_ss.ar_high, cpu_state.seg_ss.access, ESP);
    x386_common_log("    Misc.  : MSR (CS/ESP/EIP)=%04X/%08X/%08X pccache=%08X/%08X\n", msr.sysenter_cs, msr.sysenter_esp, msr.sysenter_eip, pccache, pccache2);
    x386_common_log("             EFLAGS=%04X%04X/%i 32=%i/%i ECX=%08X EDX=%08X abrt=%02X\n", cpu_state.eflags, cpu_state.flags, !!trap, !!use32, !!stack32, ECX, EDX, cpu_state.abrt);
#endif

    return 1;
}

int
sysexit(UNUSED(uint32_t fetchdat))
{
#ifdef ENABLE_386_COMMON_LOG
    x386_common_log("SYSEXIT called\n");
#endif

    if (!(msr.sysenter_cs & 0xFFF8)) {
#ifdef ENABLE_386_COMMON_LOG
        x386_common_log("SYSEXIT: CS MSR is zero");
#endif
        x86gpf("SYSEXIT: CS MSR is zero", 0);
        return cpu_state.abrt;
    }

    if (!(msw & 1)) {
#ifdef ENABLE_386_COMMON_LOG
        x386_common_log("SYSEXIT: CPU not in protected mode");
#endif
        x86gpf("SYSEXIT: CPU not in protected mode", 0);
        return cpu_state.abrt;
    }

    if (CPL) {
#ifdef ENABLE_386_COMMON_LOG
        x386_common_log("SYSEXIT: CPL not 0");
#endif
        x86gpf("SYSEXIT: CPL not 0", 0);
        return cpu_state.abrt;
    }

#ifdef ENABLE_386_COMMON_LOG
    x386_common_log("SYSEXIT start:\n");
    x386_common_log("    CS %04X/%i: b=%08X l=%08X (%08X-%08X) a=%02X%02X; EIP=%08X\n", cpu_state.seg_cs.seg, !!cpu_state.seg_cs.checked, cpu_state.seg_cs.base, cpu_state.seg_cs.limit, cpu_state.seg_cs.limit_low, cpu_state.seg_cs.limit_high, cpu_state.seg_cs.ar_high, cpu_state.seg_cs.access, cpu_state.pc);
    x386_common_log("    SS %04X/%i: b=%08X l=%08X (%08X-%08X) a=%02X%02X; ESP=%08X\n", cpu_state.seg_ss.seg, !!cpu_state.seg_ss.checked, cpu_state.seg_ss.base, cpu_state.seg_ss.limit, cpu_state.seg_ss.limit_low, cpu_state.seg_ss.limit_high, cpu_state.seg_ss.ar_high, cpu_state.seg_ss.access, ESP);
    x386_common_log("    Misc.  : MSR (CS/ESP/EIP)=%04X/%08X/%08X pccache=%08X/%08X\n", msr.sysenter_cs, msr.sysenter_esp, msr.sysenter_eip, pccache, pccache2);
    x386_common_log("             EFLAGS=%04X%04X/%i 32=%i/%i ECX=%08X EDX=%08X abrt=%02X\n", cpu_state.eflags, cpu_state.flags, !!trap, !!use32, !!stack32, ECX, EDX, cpu_state.abrt);
#endif

#ifndef USE_NEW_DYNAREC
    oldcs = CS;
#endif
    cpu_state.oldpc = cpu_state.pc;
    ESP             = ECX;
    cpu_state.pc    = EDX;

    cpu_state.seg_cs.seg        = (((msr.sysenter_cs + 16) & 0xfffc) | 3);
    cpu_state.seg_cs.base       = 0;
    cpu_state.seg_cs.limit_low  = 0;
    cpu_state.seg_cs.limit      = 0xffffffff;
    cpu_state.seg_cs.limit_high = 0xffffffff;
    cpu_state.seg_cs.access     = 0xfb;
    cpu_state.seg_cs.ar_high    = 0xcf;
    cpu_state.seg_cs.checked    = 1;
    oldcpl                      = 3;

    cpu_state.seg_ss.seg        = (((msr.sysenter_cs + 24) & 0xfffc) | 3);
    cpu_state.seg_ss.base       = 0;
    cpu_state.seg_ss.limit_low  = 0;
    cpu_state.seg_ss.limit      = 0xffffffff;
    cpu_state.seg_ss.limit_high = 0xffffffff;
    cpu_state.seg_ss.access     = 0xf3;
    cpu_state.seg_ss.ar_high    = 0xcf;
    cpu_state.seg_ss.checked    = 1;
#ifdef USE_DYNAREC
    codegen_flat_ss = 0;
#endif

    cpu_cur_status &= ~(CPU_STATUS_NOTFLATSS /* | CPU_STATUS_V86*/);
    cpu_cur_status |= (CPU_STATUS_USE32 | CPU_STATUS_STACK32 | CPU_STATUS_PMODE);
    flushmmucache_nopc();
    set_use32(1);
    set_stack32(1);

    in_sys = 0;

#ifdef ENABLE_386_COMMON_LOG
    x386_common_log("SYSEXIT completed:\n");
    x386_common_log("    CS %04X/%i: b=%08X l=%08X (%08X-%08X) a=%02X%02X; EIP=%08X\n", cpu_state.seg_cs.seg, !!cpu_state.seg_cs.checked, cpu_state.seg_cs.base, cpu_state.seg_cs.limit, cpu_state.seg_cs.limit_low, cpu_state.seg_cs.limit_high, cpu_state.seg_cs.ar_high, cpu_state.seg_cs.access, cpu_state.pc);
    x386_common_log("    SS %04X/%i: b=%08X l=%08X (%08X-%08X) a=%02X%02X; ESP=%08X\n", cpu_state.seg_ss.seg, !!cpu_state.seg_ss.checked, cpu_state.seg_ss.base, cpu_state.seg_ss.limit, cpu_state.seg_ss.limit_low, cpu_state.seg_ss.limit_high, cpu_state.seg_ss.ar_high, cpu_state.seg_ss.access, ESP);
    x386_common_log("    Misc.  : MSR (CS/ESP/EIP)=%04X/%08X/%08X pccache=%08X/%08X\n", msr.sysenter_cs, msr.sysenter_esp, msr.sysenter_eip, pccache, pccache2);
    x386_common_log("             EFLAGS=%04X%04X/%i 32=%i/%i ECX=%08X EDX=%08X abrt=%02X\n", cpu_state.eflags, cpu_state.flags, !!trap, !!use32, !!stack32, ECX, EDX, cpu_state.abrt);
#endif

    return 1;
}

int
syscall_op(UNUSED(uint32_t fetchdat))
{
#ifdef ENABLE_386_COMMON_LOG
    x386_common_log("SYSCALL called\n");
#endif

    /* Let's do this by the AMD spec. */
    /* Set VM and IF to 0. */
    cpu_state.eflags &= ~VM_FLAG;
    cpu_state.flags &= ~I_FLAG;

#ifndef USE_NEW_DYNAREC
    oldcs = CS;
#endif
    cpu_state.oldpc = cpu_state.pc;
    ECX             = cpu_state.pc;

    /* CS */
    CS                          = AMD_SYSCALL_SB & 0xfffc;
    cpu_state.seg_cs.base       = 0;
    cpu_state.seg_cs.limit_low  = 0;
    cpu_state.seg_cs.limit      = 0xffffffff;
    cpu_state.seg_cs.limit_high = 0xffffffff;
    cpu_state.seg_cs.access     = 0x9b;
    cpu_state.seg_cs.ar_high    = 0xcf;
    cpu_state.seg_cs.checked    = 1;
    oldcpl                      = 0;

    /* SS */
    SS                          = (AMD_SYSCALL_SB + 8) & 0xfffc;
    cpu_state.seg_ss.base       = 0;
    cpu_state.seg_ss.limit_low  = 0;
    cpu_state.seg_ss.limit      = 0xffffffff;
    cpu_state.seg_ss.limit_high = 0xffffffff;
    cpu_state.seg_ss.access     = 0x93;
    cpu_state.seg_ss.ar_high    = 0xcf;
    cpu_state.seg_ss.checked    = 1;
#ifdef USE_DYNAREC
    codegen_flat_ss = 0;
#endif

    cpu_cur_status &= ~(CPU_STATUS_NOTFLATSS | CPU_STATUS_V86);
    cpu_cur_status |= (CPU_STATUS_USE32 | CPU_STATUS_STACK32 | CPU_STATUS_PMODE);
    set_use32(1);
    set_stack32(1);

    in_sys = 1;

    return 1;
}

int
sysret(UNUSED(uint32_t fetchdat))
{
#ifdef ENABLE_386_COMMON_LOG
    x386_common_log("SYSRET called\n");
#endif

    if (CPL) {
#ifdef ENABLE_386_COMMON_LOG
        x386_common_log("SYSRET: CPL not 0");
#endif
        x86gpf("SYSRET: CPL not 0", 0);
        return cpu_state.abrt;
    }

    cpu_state.flags |= I_FLAG;
    /* First instruction after SYSRET will always execute, regardless of whether
       there is a pending interrupt, following the STI logic */
    cpu_end_block_after_ins = 2;

#ifndef USE_NEW_DYNAREC
    oldcs = CS;
#endif
    cpu_state.oldpc = cpu_state.pc;
    cpu_state.pc    = ECX;

    /* CS */
    CS                          = (AMD_SYSRET_SB & 0xfffc) | 3;
    cpu_state.seg_cs.base       = 0;
    cpu_state.seg_cs.limit_low  = 0;
    cpu_state.seg_cs.limit      = 0xffffffff;
    cpu_state.seg_cs.limit_high = 0xffffffff;
    cpu_state.seg_cs.access     = 0xfb;
    cpu_state.seg_cs.ar_high    = 0xcf;
    cpu_state.seg_cs.checked    = 1;
    oldcpl                      = 3;

    /* SS */
    SS                          = ((AMD_SYSRET_SB + 8) & 0xfffc) | 3;
    cpu_state.seg_ss.base       = 0;
    cpu_state.seg_ss.limit_low  = 0;
    cpu_state.seg_ss.limit      = 0xffffffff;
    cpu_state.seg_ss.limit_high = 0xffffffff;
    cpu_state.seg_ss.access     = 0xf3;
    cpu_state.seg_cs.ar_high    = 0xcf;
    cpu_state.seg_ss.checked    = 1;
#ifdef USE_DYNAREC
    codegen_flat_ss = 0;
#endif

    cpu_cur_status &= ~(CPU_STATUS_NOTFLATSS /* | CPU_STATUS_V86*/);
    cpu_cur_status |= (CPU_STATUS_USE32 | CPU_STATUS_STACK32 | CPU_STATUS_PMODE);
    flushmmucache_nopc();
    set_use32(1);
    set_stack32(1);

    in_sys = 0;

    return 1;
}

void
cpu_register_fast_off_handler(void *timer)
{
    cpu_fast_off_timer = (pc_timer_t *) timer;
}

void
cpu_fast_off_advance(void)
{
    timer_disable(cpu_fast_off_timer);
    if (cpu_fast_off_period != 0.0)
        timer_on_auto(cpu_fast_off_timer, cpu_fast_off_period);
}

void
cpu_fast_off_period_set(uint16_t val, double period)
{
    cpu_fast_off_period = ((double) (val + 1)) * period;
    cpu_fast_off_advance();
}

void
cpu_fast_off_reset(void)
{
    cpu_register_fast_off_handler(NULL);
    cpu_fast_off_period = 0.0;
    cpu_fast_off_advance();
}

void
smi_raise(void)
{
    uint8_t ccr1_check;

    if (is_cx6x86)
        ccr1_check = ((ccr1 & (CCR1_USE_SMI | CCR1_SMAC | CCR1_SM3)) ==
                      (CCR1_USE_SMI | CCR1_SM3)) && (cyrix.arr[3].size > 0);
    else
        ccr1_check = ((ccr1 & (CCR1_USE_SMI | CCR1_SMAC)) ==
                      (CCR1_USE_SMI)) && (cyrix.arr[3].size > 0);

    if (is_cxsmm && !ccr1_check)
        return;

    if (is486 && (cpu_fast_off_flags & 0x80000000))
        cpu_fast_off_advance();

    smi_line = 1;
}

void
nmi_raise(void)
{
    if (is486 && (cpu_fast_off_flags & 0x20000000))
        cpu_fast_off_advance();

    if (!strcmp(machine_get_internal_name(), "ibmtp700"))
        pclog("TP700 NMI RAISE: at %04X:%08X mask=%02X\n",
              CS, cpu_state.pc, nmi_mask);
    nmi = 1;
}

#ifndef USE_DYNAREC
/* This is for compatibility with new x87 code. */
void
codegen_set_rounding_mode(int mode)
{
    /* cpu_state.new_npxc = (cpu_state.old_npxc & ~0xc00) | (mode << 10); */
}
#endif
