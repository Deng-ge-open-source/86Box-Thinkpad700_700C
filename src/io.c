/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implement I/O ports and their operations.
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Miran Grca, <mgrca8@gmail.com>
 *          Fred N. van Kempen, <decwiz@yahoo.com>
 *
 *          Copyright 2008-2019 Sarah Walker.
 *          Copyright 2016-2025 Miran Grca.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/timer.h>
#include <86box/video.h>
#include <86box/vid_svga.h>
#include "cpu.h"
#include "x86.h"
#include <86box/m_amstrad.h>
#include <86box/machine.h>
#include <86box/pci.h>

#define NPORTS 65536 /* PC/AT supports 64K ports */

typedef struct _io_ {
    uint8_t (*inb)(uint16_t port, void *priv);
    uint16_t (*inw)(uint16_t port, void *priv);
    uint32_t (*inl)(uint16_t port, void *priv);

    void (*outb)(uint16_t port, uint8_t val, void *priv);
    void (*outw)(uint16_t port, uint16_t val, void *priv);
    void (*outl)(uint16_t port, uint32_t val, void *priv);

    void *priv;

    struct _io_ *prev, *next;
} io_t;

typedef struct io_trap_s {
    uint8_t   enable;
    uint16_t  base;
    uint16_t  size;
    void    (*func)(uint16_t size, uint16_t port, uint8_t write, uint8_t val, void *priv);
    void     *priv;
} io_trap_t;

uint8_t initialized = 0;
io_t   *io[NPORTS];
io_t   *io_last[NPORTS];

static uint64_t tp700_io_events = 0;
static uint64_t tp700_kbc_events = 0;

/*
 * The 9552 firmware begins its base-memory test at POST checkpoint 51h/52h
 * and leaves the completed count visible through the rest of POST.  The
 * desktop Model 50/70 BIOS writes its count into VGA text RAM;
 * do the same at the machine boundary instead of painting a host-side SDL
 * overlay.  Reference-disk mode changes and clears now naturally own the
 * resulting pixels, so POST text cannot leak into a later screen.
 */
static int tp700_post_memory_active = 0;

int
tp700_post_memory_display_active(void)
{
    return tp700_post_memory_active;
}

void
tp700_post_memory_display_finish(void)
{
    tp700_post_memory_active = 0;
}

static void
tp700_post_video_init(void)
{
    static const uint8_t seq[5] = {
        0x03, 0x00, 0x03, 0x00, 0x02
    };
    static const uint8_t crtc[25] = {
        0x5f, 0x4f, 0x50, 0x82, 0x55, 0x81, 0xbf, 0x1f,
        0x00, 0x4f, 0x0d, 0x0e, 0x00, 0x00, 0x00, 0x50,
        0x9c, 0x0e, 0x8f, 0x28, 0x1f, 0x96, 0xb9, 0xa3,
        0xff
    };
    static const uint8_t gdc[9] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0e, 0x00, 0xff
    };
    static const uint8_t attr[21] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x0c, 0x00, 0x0f, 0x08, 0x00
    };
    svga_t *svga = svga_get_pri();

    if ((svga == NULL) || (svga->video_out == NULL))
        return;

    /*
     * The 9552's WD90C26 LCD adapter exposes an 80x25 status screen before
     * the protected-mode memory test.  The temporary Model-50 VGA base starts
     * blank, so program the equivalent standard VGA text timing here.  This
     * is guest video state: later BIOS/reference-disk mode sets replace it in
     * the normal way.
     */
    svga->video_out(0x03c2, 0x67, svga->priv);
    for (uint8_t i = 0; i < 5; i++) {
        svga->video_out(0x03c4, i, svga->priv);
        svga->video_out(0x03c5, seq[i], svga->priv);
    }

    svga->video_out(0x03d4, 0x11, svga->priv);
    svga->video_out(0x03d5, crtc[0x11] & 0x7f, svga->priv);
    for (uint8_t i = 0; i < 25; i++) {
        svga->video_out(0x03d4, i, svga->priv);
        svga->video_out(0x03d5, crtc[i], svga->priv);
    }
    for (uint8_t i = 0; i < 9; i++) {
        svga->video_out(0x03ce, i, svga->priv);
        svga->video_out(0x03cf, gdc[i], svga->priv);
    }
    for (uint8_t i = 0; i < 21; i++) {
        (void) svga_in(0x03da, svga);
        svga->video_out(0x03c0, i, svga->priv);
        svga->video_out(0x03c0, attr[i], svga->priv);
    }
    (void) svga_in(0x03da, svga);
    svga->video_out(0x03c0, 0x20, svga->priv);

    /* Populate plane 2 with the same IBM 8x16 font used by normal VGA text. */
    for (uint16_t ch = 0; ch < 256; ch++) {
        for (uint8_t row = 0; row < 16; row++)
            svga->vram[2 + (ch * 128) + (row << 2)] = fontdatm[ch][row];
    }

    svga->dac_mask   = 0xff;
    svga->pallook[0] = 0x00000000;
    svga->pallook[7] = 0x00c0c0c0;
    svga->pallook[15] = 0x00ffffff;
    svga->fullchange = changeframecount;
    svga_recalctimings(svga);

    /* Clear all 80x25 text cells before the first count/checkpoint. */
    for (uint32_t cell = 0; cell < (80 * 25); cell++) {
        mem_writeb_phys(0x000b8000 + (cell << 1), ' ');
        mem_writeb_phys(0x000b8000 + (cell << 1) + 1, 0x07);
    }

    /* Match the BIOS data-area state of an 80x25 color-text mode.  Without
       the 9552's WD90C26 video BIOS, the inherited base-machine values still
       describe a 40-column screen and the ROM's native status text wraps. */
    ram[0x0449]                = 0x03;
    *(uint16_t *) &ram[0x044a] = 80;
    *(uint16_t *) &ram[0x044c] = 0x1000;
    memset(&ram[0x0450], 0, 16);
    ram[0x0462]                = 0;
    *(uint16_t *) &ram[0x0463] = 0x03d4;
    ram[0x0465]                = 0x29;
    ram[0x0466]                = 0;
}

#ifdef ENABLE_IO_LOG
uint8_t io_do_log = ENABLE_IO_LOG;

static void
io_log(const char *fmt, ...)
{
    va_list ap;

    if (io_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define io_log(fmt, ...)
#endif

void
io_init(void)
{
    io_t *p;
    io_t *q;

    if (!initialized) {
        for (uint32_t c = 0; c < NPORTS; c++)
            io[c] = io_last[c] = NULL;
        initialized = 1;
    }

    for (uint32_t c = 0; c < NPORTS; c++) {
        if (io_last[c]) {
            /* Port c has at least one handler. */
            p = io_last[c];
            /* After this loop, p will have the pointer to the first handler. */
            while (p) {
                q = p->prev;
                free(p);
                p = q;
            }
            p = NULL;
        }

        /* io[c] should be NULL. */
        io[c] = io_last[c] = NULL;
    }
}

void
io_sethandler_common(uint16_t base, uint16_t size,
                     uint8_t (*inb)(uint16_t port, void *priv),
                     uint16_t (*inw)(uint16_t port, void *priv),
                     uint32_t (*inl)(uint16_t port, void *priv),
                     void (*outb)(uint16_t port, uint8_t val, void *priv),
                     void (*outw)(uint16_t port, uint16_t val, void *priv),
                     void (*outl)(uint16_t port, uint32_t val, void *priv),
                     void *priv, uint8_t step)
{
    io_t *p;
    io_t *q = NULL;

    for (uint32_t c = 0; c < size; c += step) {
        p = io_last[base + c];
        q = (io_t *) calloc(1, sizeof(io_t));
        if (p) {
            p->next = q;
            q->prev = p;
        } else {
            io[base + c] = q;
            q->prev      = NULL;
        }

        q->inb = inb;
        q->inw = inw;
        q->inl = inl;

        q->outb = outb;
        q->outw = outw;
        q->outl = outl;

        q->priv = priv;
        q->next = NULL;

        io_last[base + c] = q;

        q = NULL;
    }
}

void
io_removehandler_common(uint16_t base, uint16_t size,
                        uint8_t (*inb)(uint16_t port, void *priv),
                        uint16_t (*inw)(uint16_t port, void *priv),
                        uint32_t (*inl)(uint16_t port, void *priv),
                        void (*outb)(uint16_t port, uint8_t val, void *priv),
                        void (*outw)(uint16_t port, uint16_t val, void *priv),
                        void (*outl)(uint16_t port, uint32_t val, void *priv),
                        void *priv, uint8_t step)
{
    io_t *p;
    io_t *q;

    for (uint32_t c = 0; c < size; c += step) {
        p = io[base + c];
        if (!p)
            continue;
        while (p) {
            q = p->next;
            if ((p->inb == inb) && (p->inw == inw) && (p->inl == inl) && (p->outb == outb) && (p->outw == outw) && (p->outl == outl) && (p->priv == priv)) {
                if (p->prev)
                    p->prev->next = p->next;
                else
                    io[base + c] = p->next;
                if (p->next)
                    p->next->prev = p->prev;
                else
                    io_last[base + c] = p->prev;
                free(p);
                p = NULL;
                break;
            }
            p = q;
        }
    }
}

void
io_handler_common(uint8_t set, uint16_t base, uint16_t size,
                  uint8_t (*inb)(uint16_t port, void *priv),
                  uint16_t (*inw)(uint16_t port, void *priv),
                  uint32_t (*inl)(uint16_t port, void *priv),
                  void (*outb)(uint16_t port, uint8_t val, void *priv),
                  void (*outw)(uint16_t port, uint16_t val, void *priv),
                  void (*outl)(uint16_t port, uint32_t val, void *priv),
                  void *priv, uint8_t step)
{
    if (set)
        io_sethandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, step);
    else
        io_removehandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, step);
}

void
io_sethandler(uint16_t base, uint16_t size,
              uint8_t (*inb)(uint16_t port, void *priv),
              uint16_t (*inw)(uint16_t port, void *priv),
              uint32_t (*inl)(uint16_t port, void *priv),
              void (*outb)(uint16_t port, uint8_t val, void *priv),
              void (*outw)(uint16_t port, uint16_t val, void *priv),
              void (*outl)(uint16_t port, uint32_t val, void *priv),
              void *priv)
{
    io_sethandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, 1);
}

void
io_removehandler(uint16_t base, uint16_t size,
                 uint8_t (*inb)(uint16_t port, void *priv),
                 uint16_t (*inw)(uint16_t port, void *priv),
                 uint32_t (*inl)(uint16_t port, void *priv),
                 void (*outb)(uint16_t port, uint8_t val, void *priv),
                 void (*outw)(uint16_t port, uint16_t val, void *priv),
                 void (*outl)(uint16_t port, uint32_t val, void *priv),
                 void *priv)
{
    io_removehandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, 1);
}

void
io_handler(uint8_t set, uint16_t base, uint16_t size,
           uint8_t (*inb)(uint16_t port, void *priv),
           uint16_t (*inw)(uint16_t port, void *priv),
           uint32_t (*inl)(uint16_t port, void *priv),
           void (*outb)(uint16_t port, uint8_t val, void *priv),
           void (*outw)(uint16_t port, uint16_t val, void *priv),
           void (*outl)(uint16_t port, uint32_t val, void *priv),
           void *priv)
{
    io_handler_common(set, base, size, inb, inw, inl, outb, outw, outl, priv, 1);
}

void
io_sethandler_interleaved(uint16_t base, uint16_t size,
                          uint8_t (*inb)(uint16_t port, void *priv),
                          uint16_t (*inw)(uint16_t port, void *priv),
                          uint32_t (*inl)(uint16_t port, void *priv),
                          void (*outb)(uint16_t port, uint8_t val, void *priv),
                          void (*outw)(uint16_t port, uint16_t val, void *priv),
                          void (*outl)(uint16_t port, uint32_t val, void *priv),
                          void *priv)
{
    io_sethandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, 2);
}

void
io_removehandler_interleaved(uint16_t base, uint16_t size,
                             uint8_t (*inb)(uint16_t port, void *priv),
                             uint16_t (*inw)(uint16_t port, void *priv),
                             uint32_t (*inl)(uint16_t port, void *priv),
                             void (*outb)(uint16_t port, uint8_t val, void *priv),
                             void (*outw)(uint16_t port, uint16_t val, void *priv),
                             void (*outl)(uint16_t port, uint32_t val, void *priv),
                             void *priv)
{
    io_removehandler_common(base, size, inb, inw, inl, outb, outw, outl, priv, 2);
}

void
io_handler_interleaved(uint8_t set, uint16_t base, uint16_t size,
                       uint8_t (*inb)(uint16_t port, void *priv),
                       uint16_t (*inw)(uint16_t port, void *priv),
                       uint32_t (*inl)(uint16_t port, void *priv),
                       void (*outb)(uint16_t port, uint8_t val, void *priv),
                       void (*outw)(uint16_t port, uint16_t val, void *priv),
                       void (*outl)(uint16_t port, uint32_t val, void *priv),
                       void *priv)
{
    io_handler_common(set, base, size, inb, inw, inl, outb, outw, outl, priv, 2);
}

#ifdef USE_DEBUG_REGS_486
extern int trap;
/* Set trap for I/O address breakpoints. */
void
io_debug_check_addr(uint16_t port)
{
    uint8_t set_trap = 0;

    if (!(dr[7] & 0xFF))
        return;
    
    if (!(cr4 & 0x8))
        return; /* No I/O debug trap. */

    for (uint8_t i = 0; i < 4; i++) {
        uint16_t dr_addr = dr[i] & 0xFFFF;
        uint8_t  breakpoint_enabled = !!(dr[7] & (0x3 << (2 * i)));
        int len_type_pair = ((dr[7] >> 16) & (0xF << (4 * i))) >> (4 * i);
        if (!breakpoint_enabled)
            continue;
        if ((len_type_pair & 3) != 2)
            continue;
        
        switch ((len_type_pair >> 2) & 3) {
            case 0x00:
                if (dr_addr == port) {
                    set_trap = 1;
                    dr[6] |= (1 << i);
                }
                break;
            case 0x01:
                if ((dr_addr & ~1) == port || ((dr_addr & ~1) + 1) == (port + 1)) {
                    set_trap = 1;
                    dr[6] |= (1 << i);
                }
                break;
            case 0x03:
                dr_addr &= ~3;
                if (port >= dr_addr && port < (dr_addr + 4)) {
                    set_trap = 1;
                    dr[6] |= (1 << i);
                }
                break;
        }
    }
    if (set_trap)
        trap |= 4;
}
#endif

uint8_t
inb(uint16_t port)
{
    uint8_t ret = 0xff;
    io_t   *p;
    io_t   *q;
    uint8_t found  = 0;
#ifdef ENABLE_IO_LOG
    uint8_t qfound = 0;
#endif

    io_port = port;

#ifdef USE_DEBUG_REGS_486
    io_debug_check_addr(port);
#endif

    if ((pci_flags & FLAG_CONFIG_IO_ON) && (port >= pci_base) && (port < (pci_base + pci_size))) {
        ret = pci_read(port, NULL);
        found = 1;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else if ((pci_flags & FLAG_CONFIG_DEV0_IO_ON) && (port >= 0xc000) && (port < 0xc100)) {
        ret = pci_read(port, NULL);
        found = 1;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else {
        p = io[port];
        while (p) {
            q = p->next;
            if (p->inb) {
                ret &= p->inb(port, p->priv);
                found |= 1;
#ifdef ENABLE_IO_LOG
                qfound++;
#endif
            }
            p = q;
        }
    }

    if (amstrad_latch & 0x80000000) {
        if (port & 0x80)
            amstrad_latch = AMSTRAD_NOLATCH | 0x80000000;
        else if (port & 0x4000)
            amstrad_latch = AMSTRAD_SW10 | 0x80000000;
        else
            amstrad_latch = AMSTRAD_SW9 | 0x80000000;
    }

    if (!found)
        cycles -= io_delay;

    /* TriGem 486-BIOS MHz output. */
#if 0
    if (port == 0x1ed)
        ret = 0xfe;
#endif

    io_log("[%04X:%08X] (%i, %i, %04i) in b(%04X) = %02X\n", CS, cpu_state.pc, in_smm, found, qfound, port, ret);

    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
        ((tp700_io_events++ < 128) ||
         (port == 0x0092) || (port == 0x0094) || (port == 0x0096) ||
         ((port >= 0x00b0) && (port <= 0x00b2)) ||
         (port == 0x00e0) || (port == 0x00e2) || (port == 0x00e4) ||
         (((port == 0x0060) || (port == 0x0064)) &&
          (tp700_kbc_events++ < 128)) ||
         ((port >= 0x0100) && (port <= 0x0107))))
        pclog("TP700 I/O: %04X:%08X INB  %04X -> %02X handled=%d\n",
              CS, cpu_state.pc, port, ret, found);

    return ret;
}

void
outb(uint16_t port, uint8_t val)
{
    io_t   *p;
    io_t   *q;
    uint8_t found  = 0;
#ifdef ENABLE_IO_LOG
    uint8_t qfound = 0;
#endif

    io_port = port;
    io_val  = val;

#ifdef USE_DEBUG_REGS_486
    io_debug_check_addr(port);
#endif

    if ((pci_flags & FLAG_CONFIG_IO_ON) && (port >= pci_base) && (port < (pci_base + pci_size))) {
        pci_write(port, val, NULL);
        found = 1;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else if ((pci_flags & FLAG_CONFIG_DEV0_IO_ON) && (port >= 0xc000) && (port < 0xc100)) {
        pci_write(port, val, NULL);
        found = 1;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else {
        p = io[port];
        while (p) {
            q = p->next;
            if (p->outb) {
                p->outb(port, val, p->priv);
                found |= 1;
#ifdef ENABLE_IO_LOG
                qfound++;
#endif
            }
            p = q;
        }
    }

    if (!found || (port == 0x84)) {
        cycles -= io_delay;
#ifdef USE_DYNAREC
        if (cpu_use_dynarec && ((port == 0x84) || (port == 0xeb) || (port == 0xed)))
            update_tsc();
#endif
    }

    io_log("[%04X:%08X] (%i, %i, %04i) outb(%04X, %02X)\n", CS, cpu_state.pc, in_smm, found, qfound, port, val);

    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
        ((tp700_io_events++ < 128) ||
         (port == 0x0084) || (port == 0x008f) || (port == 0x0092) || (port == 0x0094) ||
         (port == 0x0096) || (port == 0x00e0) || (port == 0x00e2) ||
         (port == 0x00e4) ||
         (((port == 0x0060) || (port == 0x0064)) &&
          (tp700_kbc_events++ < 128)) ||
         ((port >= 0x00b0) && (port <= 0x00b2)) ||
         ((port >= 0x0100) && (port <= 0x0107)) ||
         (port == 0x03bc) || (port == 0x0680)))
        pclog("TP700 I/O: %04X:%08X OUTB %04X <- %02X handled=%d\n",
              CS, cpu_state.pc, port, val, found);

    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
        (port == 0x0680) && ((val == 0xb0) || (val == 0xb9))) {
        const uint32_t save = ds + (ESI & 0xffff) + 0xb0;

        pclog("TP700 STACKSAVE: checkpoint=%02X DS:SI=%04X:%04X "
              "SS:SP=%04X:%04X AX=%04X FLAGS=%04X save=%08X "
              "values=%02X%02X %02X%02X %02X%02X field39=%02X%02X\n",
              val, DS, ESI & 0xffff, SS, ESP & 0xffff,
              AX, cpu_state.flags, save,
              ram[save + 1], ram[save], ram[save + 3], ram[save + 2],
              ram[save + 5], ram[save + 4],
              ram[ds + (ESI & 0xffff) + 0x3a],
              ram[ds + (ESI & 0xffff) + 0x39]);
    }

    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
        (port == 0x0680)) {
        /* A0h is the first 9552 power-on checkpoint.  Make the emulated LCD
           text path available before the ROM prints either the CP prefix or
           the memory count; initializing at 51h is already too late. */
        if (val == 0xa0) {
            tp700_post_video_init();
            tp700_post_memory_active = 0;
        }

        if ((val == 0x51) && !tp700_post_memory_active) {
            tp700_post_memory_active = 1;
            pclog("TP700 CHECKPOINT51 STACK: SS:SP=%04X:%04X "
                  "%04X %04X %04X %04X %04X %04X %04X %04X "
                  "AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X\n",
                  SS, SP,
                  readmemwl(ss + SP),
                  readmemwl(ss + ((SP + 2) & 0xffff)),
                  readmemwl(ss + ((SP + 4) & 0xffff)),
                  readmemwl(ss + ((SP + 6) & 0xffff)),
                  readmemwl(ss + ((SP + 8) & 0xffff)),
                  readmemwl(ss + ((SP + 10) & 0xffff)),
                  readmemwl(ss + ((SP + 12) & 0xffff)),
                  readmemwl(ss + ((SP + 14) & 0xffff)),
                  AX, BX, CX, DX, SI, DI, BP);
        }

    }

    if (!strcmp(machine_get_internal_name(), "ibmtp700") &&
        (port == 0x0680) && (val == 0xf0) &&
        (*(uint16_t *) &ram[0x0413] > 0x0280)) {
        /*
         * The 9552 ROM normally creates the initial 1 KiB EBDA while leaving
         * POST protected mode.  The Model 50 memory controller used as the
         * current base does not provide that transition, so the test pattern
         * (FFFFh) survives in the BDA and makes the later IML control block
         * overwrite F000 ROM.  Recreate the architectural postcondition here:
         * 639 KiB conventional memory plus a 1 KiB EBDA at 9FC0h.
         */
        memset(&ram[0x0009fc00], 0, 0x400);
        ram[0x0009fc00]              = 1;
        *(uint16_t *) &ram[0x040e]   = 0x9fc0;
        *(uint16_t *) &ram[0x0413]   = 0x027f;
        pclog("TP700 BDA FIXUP: base=027F EBDA=9FC0\n");
    }

    if (0 && !strcmp(machine_get_internal_name(), "ibmtp700") &&
        (port == 0x0680))
        pclog("TP700 BDA: checkpoint=%02X base=%04X EBDA=%04X "
              "F7801=%02X\n",
              val, *(uint16_t *) &ram[0x0413],
              *(uint16_t *) &ram[0x040e], ram[0xf7801]);

    return;
}

uint16_t
inw(uint16_t port)
{
    io_t    *p;
    io_t    *q;
    uint16_t ret    = 0xffff;
    uint8_t  found  = 0;
#ifdef ENABLE_IO_LOG
    uint8_t  qfound = 0;
#endif
    uint8_t  ret8[2];

    io_port = port;

#ifdef USE_DEBUG_REGS_486
    io_debug_check_addr(port);
#endif

    if ((pci_flags & FLAG_CONFIG_IO_ON) && (port >= pci_base) && (port < (pci_base + pci_size))) {
        ret = pci_readw(port, NULL);
        found = 2;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else if ((pci_flags & FLAG_CONFIG_DEV0_IO_ON) && (port >= 0xc000) && (port < 0xc100)) {
        ret = pci_readw(port, NULL);
        found = 2;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else {
        p = io[port];
        while (p) {
            q = p->next;
            if (p->inw) {
                ret &= p->inw(port, p->priv);
                found |= 2;
#ifdef ENABLE_IO_LOG
                qfound++;
#endif
            }
            p = q;
        }

        ret8[0] = ret & 0xff;
        ret8[1] = (ret >> 8) & 0xff;
        for (uint8_t i = 0; i < 2; i++) {
            p = io[(port + i) & 0xffff];
            while (p) {
                q = p->next;
                if (p->inb && !p->inw) {
                    ret8[i] &= p->inb(port + i, p->priv);
                    found |= 1;
#ifdef ENABLE_IO_LOG
                    qfound++;
#endif
                }
                p = q;
            }
        }
        ret = (ret8[1] << 8) | ret8[0];
    }

    if (amstrad_latch & 0x80000000) {
        if (port & 0x80)
            amstrad_latch = AMSTRAD_NOLATCH | 0x80000000;
        else if (port & 0x4000)
            amstrad_latch = AMSTRAD_SW10 | 0x80000000;
        else
            amstrad_latch = AMSTRAD_SW9 | 0x80000000;
    }

    if (!found)
        cycles -= io_delay;

    io_log("[%04X:%08X] (%i, %i, %04i) in w(%04X) = %04X\n", CS, cpu_state.pc, in_smm, found, qfound, port, ret);

    return ret;
}

void
outw(uint16_t port, uint16_t val)
{
    io_t   *p;
    io_t   *q;
    uint8_t found  = 0;
#ifdef ENABLE_IO_LOG
    uint8_t qfound = 0;
#endif

    io_port = port;
    io_val  = val;

#ifdef USE_DEBUG_REGS_486
    io_debug_check_addr(port);
#endif

    if ((pci_flags & FLAG_CONFIG_IO_ON) && (port >= pci_base) && (port < (pci_base + pci_size))) {
        pci_writew(port, val, NULL);
        found = 2;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else if ((pci_flags & FLAG_CONFIG_DEV0_IO_ON) && (port >= 0xc000) && (port < 0xc100)) {
        pci_writew(port, val, NULL);
        found = 2;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else {
        p = io[port];
        while (p) {
            q = p->next;
            if (p->outw) {
                p->outw(port, val, p->priv);
                found |= 2;
#ifdef ENABLE_IO_LOG
                qfound++;
#endif
            }
            p = q;
        }

        for (uint8_t i = 0; i < 2; i++) {
            p = io[(port + i) & 0xffff];
            while (p) {
                q = p->next;
                if (p->outb && !p->outw) {
                    p->outb(port + i, val >> (i << 3), p->priv);
                    found |= 1;
#ifdef ENABLE_IO_LOG
                    qfound++;
#endif
                }
                p = q;
            }
        }
    }

    if (!found) {
        cycles -= io_delay;
#ifdef USE_DYNAREC
        if (cpu_use_dynarec && ((port == 0xeb) || (port == 0xed)))
            update_tsc();
#endif
    }

    io_log("[%04X:%08X] (%i, %i, %04i) outw(%04X, %04X)\n", CS, cpu_state.pc, in_smm, found, qfound, port, val);

    return;
}

uint32_t
inl(uint16_t port)
{
    io_t    *p;
    io_t    *q;
    uint32_t ret = 0xffffffff;
    uint16_t ret16[2];
    uint8_t  ret8[4];
    uint8_t  found  = 0;
#ifdef ENABLE_IO_LOG
    uint8_t  qfound = 0;
#endif

    io_port = port;

#ifdef USE_DEBUG_REGS_486
    io_debug_check_addr(port);
#endif

    if ((pci_flags & FLAG_CONFIG_IO_ON) && (port >= pci_base) && (port < (pci_base + pci_size))) {
        ret = pci_readl(port, NULL);
        found = 4;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else if ((pci_flags & FLAG_CONFIG_DEV0_IO_ON) && (port >= 0xc000) && (port < 0xc100)) {
        ret = pci_readl(port, NULL);
        found = 4;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else {
        p = io[port];
        while (p) {
            q = p->next;
            if (p->inl) {
                ret &= p->inl(port, p->priv);
                found |= 4;
#ifdef ENABLE_IO_LOG
                qfound++;
#endif
            }
            p = q;
        }

        ret16[0] = ret & 0xffff;
        ret16[1] = (ret >> 16) & 0xffff;
        p        = io[port & 0xffff];
        while (p) {
            q = p->next;
            if (p->inw && !p->inl) {
                ret16[0] &= p->inw(port, p->priv);
                found |= 2;
#ifdef ENABLE_IO_LOG
                qfound++;
#endif
            }
            p = q;
        }

        p = io[(port + 2) & 0xffff];
        while (p) {
            q = p->next;
            if (p->inw && !p->inl) {
                ret16[1] &= p->inw(port + 2, p->priv);
                found |= 2;
#ifdef ENABLE_IO_LOG
                qfound++;
#endif
            }
            p = q;
        }
        ret = (ret16[1] << 16) | ret16[0];

        ret8[0] = ret & 0xff;
        ret8[1] = (ret >> 8) & 0xff;
        ret8[2] = (ret >> 16) & 0xff;
        ret8[3] = (ret >> 24) & 0xff;
        for (uint8_t i = 0; i < 4; i++) {
            p = io[(port + i) & 0xffff];
            while (p) {
                q = p->next;
                if (p->inb && !p->inw && !p->inl) {
                    ret8[i] &= p->inb(port + i, p->priv);
                    found |= 1;
#ifdef ENABLE_IO_LOG
                    qfound++;
#endif
                }
                p = q;
            }
        }
        ret = (ret8[3] << 24) | (ret8[2] << 16) | (ret8[1] << 8) | ret8[0];
    }

    if (amstrad_latch & 0x80000000) {
        if (port & 0x80)
            amstrad_latch = AMSTRAD_NOLATCH | 0x80000000;
        else if (port & 0x4000)
            amstrad_latch = AMSTRAD_SW10 | 0x80000000;
        else
            amstrad_latch = AMSTRAD_SW9 | 0x80000000;
    }

    if (!found)
        cycles -= io_delay;

    io_log("[%04X:%08X] (%i, %i, %04i) in l(%04X) = %08X\n", CS, cpu_state.pc, in_smm, found, qfound, port, ret);

    return ret;
}

void
outl(uint16_t port, uint32_t val)
{
    io_t   *p;
    io_t   *q;
    uint8_t found  = 0;
#ifdef ENABLE_IO_LOG
    uint8_t qfound = 0;
#endif

    io_port = port;
    io_val  = val;

#ifdef USE_DEBUG_REGS_486
    io_debug_check_addr(port);
#endif

    if ((pci_flags & FLAG_CONFIG_IO_ON) && (port >= pci_base) && (port < (pci_base + pci_size))) {
        pci_writel(port, val, NULL);
        found = 4;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else if ((pci_flags & FLAG_CONFIG_DEV0_IO_ON) && (port >= 0xc000) && (port < 0xc100)) {
        pci_writel(port, val, NULL);
        found = 4;
#ifdef ENABLE_IO_LOG
        qfound = 1;
#endif
    } else {
        p = io[port];
        if (p) {
            while (p) {
                q = p->next;
                if (p->outl) {
                    p->outl(port, val, p->priv);
                    found |= 4;
#ifdef ENABLE_IO_LOG
                    qfound++;
#endif
                }
                p = q;
            }
        }

        for (uint8_t i = 0; i < 4; i += 2) {
            p = io[(port + i) & 0xffff];
            while (p) {
                q = p->next;
                if (p->outw && !p->outl) {
                    p->outw(port + i, val >> (i << 3), p->priv);
                    found |= 2;
#ifdef ENABLE_IO_LOG
                    qfound++;
#endif
                }
                p = q;
            }
        }

        for (uint8_t i = 0; i < 4; i++) {
            p = io[(port + i) & 0xffff];
            while (p) {
                q = p->next;
                if (p->outb && !p->outw && !p->outl) {
                    p->outb(port + i, val >> (i << 3), p->priv);
                    found |= 1;
#ifdef ENABLE_IO_LOG
                    qfound++;
#endif
                }
                p = q;
            }
        }
    }

    if (!found) {
        cycles -= io_delay;
#ifdef USE_DYNAREC
        if (cpu_use_dynarec && ((port == 0xeb) || (port == 0xed)))
            update_tsc();
#endif
    }

    io_log("[%04X:%08X] (%i, %i, %04i) outl(%04X, %08X)\n", CS, cpu_state.pc, in_smm, found, qfound, port, val);

    return;
}

static uint8_t
io_trap_readb(uint16_t port, void *priv)
{
    io_trap_t *trap = (io_trap_t *) priv;
    trap->func(1, port, 0, 0, trap->priv);
    return 0xff;
}

static uint16_t
io_trap_readw(uint16_t port, void *priv)
{
    io_trap_t *trap = (io_trap_t *) priv;
    trap->func(2, port, 0, 0, trap->priv);
    return 0xffff;
}

static uint32_t
io_trap_readl(uint16_t port, void *priv)
{
    io_trap_t *trap = (io_trap_t *) priv;
    trap->func(4, port, 0, 0, trap->priv);
    return 0xffffffff;
}

static void
io_trap_writeb(uint16_t port, uint8_t val, void *priv)
{
    io_trap_t *trap = (io_trap_t *) priv;
    trap->func(1, port, 1, val, trap->priv);
}

static void
io_trap_writew(uint16_t port, uint16_t val, void *priv)
{
    io_trap_t *trap = (io_trap_t *) priv;
    trap->func(2, port, 1, val, trap->priv);
}

static void
io_trap_writel(uint16_t port, uint32_t val, void *priv)
{
    io_trap_t *trap = (io_trap_t *) priv;
    trap->func(4, port, 1, val, trap->priv);
}

void *
io_trap_add(void (*func)(uint16_t size, uint16_t port, uint8_t write, uint8_t val, void *priv),
            void *priv)
{
    /* Instantiate new I/O trap. */
    io_trap_t *trap = (io_trap_t *) calloc(1, sizeof(io_trap_t));
    trap->enable    = 0;
    trap->base = trap->size = 0;
    trap->func              = func;
    trap->priv              = priv;

    return trap;
}

void
io_trap_remap(void *handle, uint8_t enable, uint16_t port, uint16_t size)
{
    io_trap_t *trap = (io_trap_t *) handle;
    if (!trap)
        return;

    io_log("I/O: Remapping trap from %04X-%04X (enable %d) to %04X-%04X (enable %d)\n",
           trap->base, trap->base + trap->size - 1, trap->enable, port, port + size - 1, enable);

    /* Remove old I/O mapping. */
    if (trap->enable && trap->size) {
        io_removehandler(trap->base, trap->size,
                         io_trap_readb, io_trap_readw, io_trap_readl,
                         io_trap_writeb, io_trap_writew, io_trap_writel,
                         trap);
    }

    /* Set trap enable flag, base address and size. */
    trap->enable = !!enable;
    trap->base   = port;
    trap->size   = size;

    /* Add new I/O mapping. */
    if (trap->enable && trap->size) {
        io_sethandler(trap->base, trap->size,
                      io_trap_readb, io_trap_readw, io_trap_readl,
                      io_trap_writeb, io_trap_writew, io_trap_writel,
                      trap);
    }
}

void
io_trap_remove(void *handle)
{
    io_trap_t *trap = (io_trap_t *) handle;
    if (!trap)
        return;

    /* Unmap I/O trap before freeing it. */
    io_trap_remap(trap, 0, 0, 0);

    free(trap);
}
