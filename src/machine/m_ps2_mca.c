/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of MCA-based PS/2 machines.
 *
 * Authors: Fred N. van Kempen, <decwiz@yahoo.com>
 *          Miran Grca, <mgrca8@gmail.com>
 *          Sarah Walker, <https://pcem-emulator.co.uk/>
 *
 *          Copyright 2017-2019 Fred N. van Kempen.
 *          Copyright 2016-2019 Miran Grca.
 *          Copyright 2008-2019 Sarah Walker.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free  Software  Foundation; either  version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is  distributed in the hope that it will be useful, but
 * WITHOUT   ANY  WARRANTY;  without  even   the  implied  warranty  of
 * MERCHANTABILITY  or FITNESS  FOR A PARTICULAR  PURPOSE. See  the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the:
 *
 *   Free Software Foundation, Inc.
 *   59 Temple Place - Suite 330
 *   Boston, MA 02111-1307
 *   USA.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include <86box/timer.h>
#include <86box/io.h>
#include <86box/dma.h>
#include <86box/pic.h>
#include <86box/pit.h>
#include <86box/mca.h>
#include <86box/mem.h>
#include <86box/nmi.h>
#include <86box/rom.h>
#include <86box/device.h>
#include <86box/fdd.h>
#include <86box/fdc.h>
#include <86box/fdc_ext.h>
#include <86box/nvr.h>
#include <86box/nvr_ps2.h>
#include <86box/keyboard.h>
#include <86box/lpt.h>
#include <86box/mouse.h>
#include <86box/port_6x.h>
#include <86box/port_92.h>
#include <86box/serial.h>
#include <86box/video.h>
#include <86box/vid_svga.h>
#include <86box/vid_vga.h>
#include <86box/machine.h>
#include <86box/plat_unused.h>

static struct ps2_t {
    uint8_t adapter_setup;
    uint8_t option[4];
    uint8_t pos_vga;
    uint8_t setup;
    uint8_t sys_ctrl_port_a;
    uint8_t subaddr_lo;
    uint8_t subaddr_hi;

    uint8_t memory_bank[8];

    uint8_t  io_id;
    uint16_t planar_id;

    mem_mapping_t split_mapping;
    mem_mapping_t expansion_mapping;
    mem_mapping_t cache_mapping;

    uint8_t (*planar_read)(uint16_t port);
    void    (*planar_write)(uint16_t port, uint8_t val);

    uint8_t mem_regs[3];

    uint32_t split_addr;
    uint32_t split_size;
    uint32_t split_phys;

    uint8_t mem_pos_regs[8];
    uint8_t mem_2mb_pos_regs[8];

    /* ThinkPad 700 indexed system-control register pair (E0h/E4h). */
    uint8_t tp700_sysctl_index;
    uint8_t tp700_sysctl_regs[256];
    uint8_t tp700_mem_presence_select;

    /* ThinkPad 700 system-management register block (B0h/B1h/B2h). */
    uint8_t tp700_sm_index;
    uint8_t tp700_sm_regs[256];
    uint8_t tp700_sm_status;
    uint8_t tp700_sm_command;
    uint8_t tp700_sm_packet[8];
    uint8_t tp700_sm_packet_pos;
    uint16_t tp700_sm_parameters[0x200];

    /* ThinkPad 700 planar serial-storage/status ports (78h..7Ch). */
    uint8_t tp700_ss_regs[5];
    uint8_t tp700_ss_command;
    uint16_t tp700_ss_words[64];

    int pending_cache_miss;

    serial_t *uart;
    lpt_t    *lpt;

    vga_t *mb_vga;
    int    has_e0000_hole;
} ps2;

/*
 * The 9552 can execute a shadowed copy of its 256 KiB firmware while the
 * DRAM normally hidden at C0000h-FFFFFh is relocated above installed RAM and
 * tested.  Backing both views with ram[C0000h] (as the temporary Model 50
 * implementation did) aliases the final 384 KiB POST block onto the running
 * BIOS.  The memory test then overwrites its own stack/code and warm-resets
 * forever at checkpoint 52h.  Keep the shadow store physically distinct,
 * matching the two simultaneous views provided by the 9552 planar.
 */
static uint8_t       thinkpad_700_shadow_ram[0x40000];
static mem_mapping_t thinkpad_700_shadow_mapping;

static uint8_t
thinkpad_700_shadow_read(uint32_t addr, UNUSED(void *priv))
{
    return thinkpad_700_shadow_ram[addr & 0x3ffff];
}

static uint16_t
thinkpad_700_shadow_readw(uint32_t addr, UNUSED(void *priv))
{
    return *(uint16_t *) &thinkpad_700_shadow_ram[addr & 0x3ffff];
}

static uint32_t
thinkpad_700_shadow_readl(uint32_t addr, UNUSED(void *priv))
{
    return *(uint32_t *) &thinkpad_700_shadow_ram[addr & 0x3ffff];
}

static void
thinkpad_700_shadow_write(uint32_t addr, uint8_t val, UNUSED(void *priv))
{
    thinkpad_700_shadow_ram[addr & 0x3ffff] = val;
}

static void
thinkpad_700_shadow_writew(uint32_t addr, uint16_t val, UNUSED(void *priv))
{
    *(uint16_t *) &thinkpad_700_shadow_ram[addr & 0x3ffff] = val;
}

static void
thinkpad_700_shadow_writel(uint32_t addr, uint32_t val, UNUSED(void *priv))
{
    *(uint32_t *) &thinkpad_700_shadow_ram[addr & 0x3ffff] = val;
}

void
machine_thinkpad_700_reset_shadow(void)
{
    if (!rom)
        return;

    memcpy(thinkpad_700_shadow_ram, rom, sizeof(thinkpad_700_shadow_ram));
    mem_set_mem_state_both(0x000c0000, sizeof(thinkpad_700_shadow_ram),
                           MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
    flushmmucache();
}

static uint8_t
thinkpad_700_sysctl_read(uint16_t port, UNUSED(void *priv))
{
    if (port == 0x00e0)
        return ps2.tp700_sysctl_index;

    /*
     * E3h is the planar memory-error latch.  Bits 0 and 1 report a detected
     * RAM/parity fault; they are clear on a healthy board.  Leaving this port
     * unimplemented returned FFh, so the 9552 POST faithfully took its RAM
     * failure path and produced 00201 followed by 00164 after every test.
     */
    if (port == 0x00e3)
        return 0x00;

    return ps2.tp700_sysctl_regs[ps2.tp700_sysctl_index];
}

static void
thinkpad_700_sysctl_write(uint16_t port, uint8_t val, UNUSED(void *priv))
{
    if (port == 0x00e0)
        ps2.tp700_sysctl_index = val;
    else if (port == 0x00e3)
        return; /* Writing the status port acknowledges/clears the latch. */
    else
        ps2.tp700_sysctl_regs[ps2.tp700_sysctl_index] = val;
}

static uint8_t
thinkpad_700_sm_read(uint16_t port, UNUSED(void *priv))
{
    switch (port) {
        case 0x00b0:
            return ps2.tp700_sm_index;
        case 0x00b1:
            return ps2.tp700_sm_regs[ps2.tp700_sm_index];
        case 0x00b2:
            return ps2.tp700_sm_status;
        default:
            return 0xff;
    }
}

/*
 * The 9552 system-management ASIC is not just an indexed register file.
 * Register 3 is an eight-byte command FIFO: firmware selects command 03h,
 * writes the four-byte header, selects the actual service (71h for the
 * parameter service), and writes the four-byte payload.  Completion is
 * returned in register 4 and result data in registers 8..0Bh.
 *
 * The system-management firmware accepts A0h as the normal completion code
 * for service 71h.  Keeping the parameter store in the emulated controller
 * also makes a set followed by a get behave like the physical device instead
 * of merely forcing POST to continue.
 */
static void
thinkpad_700_sm_execute_packet(void)
{
    uint16_t parameter;
    uint16_t value;

    switch (ps2.tp700_sm_command) {
        case 0x71:
            parameter = ((uint16_t) ps2.tp700_sm_packet[2] << 8) |
                        ps2.tp700_sm_packet[1];

            switch (ps2.tp700_sm_packet[0]) {
                case 0x00: /* Read a system-management parameter. */
                    value = (parameter < 0x200) ?
                            ps2.tp700_sm_parameters[parameter] : 0;
                    break;

                case 0x02: /* Write a system-management parameter. */
                    value = ((uint16_t) ps2.tp700_sm_packet[5] << 8) |
                            ps2.tp700_sm_packet[4];
                    if (parameter < 0x200)
                        ps2.tp700_sm_parameters[parameter] = value;
                    break;

                default:
                    value = 0;
                    break;
            }

            ps2.tp700_sm_regs[0x04] = 0xa0;
            ps2.tp700_sm_regs[0x08] = value & 0xff;
            ps2.tp700_sm_regs[0x09] = value >> 8;
            ps2.tp700_sm_regs[0x0a] = 0x00;
            ps2.tp700_sm_regs[0x0b] = 0x00;
            break;

        default:
            /* Unknown services complete without transport failure.  The
               service-specific response remains zero for firmware to test. */
            ps2.tp700_sm_regs[0x04] = 0x00;
            ps2.tp700_sm_regs[0x08] = 0x00;
            ps2.tp700_sm_regs[0x09] = 0x00;
            ps2.tp700_sm_regs[0x0a] = 0x00;
            ps2.tp700_sm_regs[0x0b] = 0x00;
            break;
    }
}

static void
thinkpad_700_sm_write(uint16_t port, uint8_t val, UNUSED(void *priv))
{
    switch (port) {
        case 0x00b0:
            ps2.tp700_sm_index = val;
            break;
        case 0x00b1:
            ps2.tp700_sm_regs[ps2.tp700_sm_index] = val;
            if (ps2.tp700_sm_index == 0x02) {
                if (val == 0x03) {
                    ps2.tp700_sm_packet_pos = 0;
                    memset(ps2.tp700_sm_packet, 0,
                           sizeof(ps2.tp700_sm_packet));
                } else {
                    ps2.tp700_sm_command = val;
                }
            } else if (ps2.tp700_sm_index == 0x03) {
                if (ps2.tp700_sm_packet_pos < sizeof(ps2.tp700_sm_packet))
                    ps2.tp700_sm_packet[ps2.tp700_sm_packet_pos++] = val;
                if (ps2.tp700_sm_packet_pos == sizeof(ps2.tp700_sm_packet))
                    thinkpad_700_sm_execute_packet();
            }
            break;
        case 0x00b2:
            ps2.tp700_sm_status = val;
            break;
        default:
            break;
    }
}

static uint8_t
thinkpad_700_ss_read(uint16_t port, UNUSED(void *priv))
{
    uint8_t ret;

    /* 79h is a write-only command register and reads as ready status. */
    if (port == 0x0079) {
        ret = 0x00;
        return ret;
    }

    /* With control bit 3 set, 7Bh exposes command status, not data. */
    if ((port == 0x007b) && (ps2.tp700_ss_regs[4] & 0x08)) {
        ret = 0x00;
        return ret;
    }

    ret = ps2.tp700_ss_regs[port - 0x0078];
    return ret;
}

static void
thinkpad_700_ss_write(uint16_t port, uint8_t val, UNUSED(void *priv))
{
    if (port == 0x0079) {
        ps2.tp700_ss_command = val;
        return;
    }

    if ((port == 0x007c) && (val & 0x80)) {
        const uint8_t index = ps2.tp700_ss_command & 0x3f;

        if (ps2.tp700_ss_command & 0x80) {
            ps2.tp700_ss_regs[2] = ps2.tp700_ss_words[index] >> 8;
            ps2.tp700_ss_regs[3] = ps2.tp700_ss_words[index] & 0xff;
        } else if (ps2.tp700_ss_command & 0x40) {
            ps2.tp700_ss_words[index] =
                (ps2.tp700_ss_regs[2] << 8) | ps2.tp700_ss_regs[3];
        }

        /* Complete synchronously: busy/error clear and data-ready set. */
        ps2.tp700_ss_regs[4] = (val & 0x1f) | 0x20;
        return;
    }

    ps2.tp700_ss_regs[port - 0x0078] = val;
}

/*The model 70 type 3/4 BIOS performs cache testing. Since 86Box doesn't have any
  proper cache emulation, it's faked a bit here.

  Port E2 is used for cache diagnostics. Bit 7 seems to be set on a cache miss,
  toggling bit 2 seems to clear this. The BIOS performs at least the following
  tests :

  - Disable RAM, access low 64kb (386) / 8kb (486), execute code from cache to
    access low memory and verify that there are no cache misses.
  - Write to low memory using DMA, read low memory and verify that all accesses
    cause cache misses.
  - Read low memory, verify that first access is cache miss. Read again and
    verify that second access is cache hit.

  These tests are also performed on the 486 model 70, despite there being no
  external cache on this system. Port E2 seems to control the internal cache on
  these systems. Presumably this port is connected to KEN#/FLUSH# on the 486.
  This behaviour is required to pass the timer interrupt test on the 486 version
  - the BIOS uses a fixed length loop that will terminate too early on a 486/25
  if it executes from internal cache.

  To handle this, 86Box uses some basic heuristics :
  - If cache is enabled but RAM is disabled, accesses to low memory go directly
    to cache memory.
  - Reads to cache addresses not 'valid' will set the cache miss flag, and mark
    that line as valid.
  - Cache flushes will clear the valid array.
  - DMA via the undocumented PS/2 command 0xb will clear the valid array.
  - Disabling the cache will clear the valid array.
  - Disabling the cache will also mark shadowed ROM areas as using ROM timings.
    This works around the timing loop mentioned above.
*/

static uint8_t ps2_cache[65536];
static int     ps2_cache_valid[65536 / 8];
static void    mem_encoding_update(void);
// #define ENABLE_PS2_MCA_LOG 1
#ifdef ENABLE_PS2_MCA_LOG
uint8_t ps2_mca_do_log = ENABLE_PS2_MCA_LOG;

static void
ps2_mca_log(const char *fmt, ...)
{
    va_list ap;

    if (ps2_mca_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define ps2_mca_log(fmt, ...)
#endif

static uint8_t
ps2_read_cache_ram(uint32_t addr, UNUSED(void *priv))
{
    ps2_mca_log("ps2_read_cache_ram: addr=%08x %i %04x:%04x\n", addr, ps2_cache_valid[addr >> 3], CS, cpu_state.pc);
    if (!ps2_cache_valid[addr >> 3]) {
        ps2_cache_valid[addr >> 3] = 1;
        ps2.mem_regs[2] |= 0x80;
    } else
        ps2.pending_cache_miss = 0;

    return ps2_cache[addr];
}
static uint16_t
ps2_read_cache_ramw(uint32_t addr, UNUSED(void *priv))
{
    ps2_mca_log("ps2_read_cache_ramw: addr=%08x %i %04x:%04x\n", addr, ps2_cache_valid[addr >> 3], CS, cpu_state.pc);
    if (!ps2_cache_valid[addr >> 3]) {
        ps2_cache_valid[addr >> 3] = 1;
        ps2.mem_regs[2] |= 0x80;
    } else
        ps2.pending_cache_miss = 0;

    return *(uint16_t *) &ps2_cache[addr];
}
static uint32_t
ps2_read_cache_raml(uint32_t addr, UNUSED(void *priv))
{
    ps2_mca_log("ps2_read_cache_raml: addr=%08x %i %04x:%04x\n", addr, ps2_cache_valid[addr >> 3], CS, cpu_state.pc);
    if (!ps2_cache_valid[addr >> 3]) {
        ps2_cache_valid[addr >> 3] = 1;
        ps2.mem_regs[2] |= 0x80;
    } else
        ps2.pending_cache_miss = 0;

    return *(uint32_t *) &ps2_cache[addr];
}
static void
ps2_write_cache_ram(uint32_t addr, uint8_t val, UNUSED(void *priv))
{
    ps2_mca_log("ps2_write_cache_ram: addr=%08x val=%02x %04x:%04x %i\n", addr, val, CS, cpu_state.pc);
    ps2_cache[addr] = val;
}

void
ps2_cache_clean(void)
{
    memset(ps2_cache_valid, 0, sizeof(ps2_cache_valid));

    /*
     * A PS/2 DMA bus-master write asserts HOLD.  On IBM's 486SLC systems the
     * processor is configured to flush its internal cache when ownership of
     * the bus changes, because the SLC has no per-line external snooping.
     * Keep the CPU's tag model coherent with the existing planar-cache
     * invalidation.  Without this, the 9552 POST sees a false hit after its
     * DMA coherency test and records 12902.
     */
    cpu_ibm_slc_cache_invalidate();
}

static uint8_t
ps2_read_split_ram(uint32_t addr, void *priv)
{
    addr = (addr % (ps2.split_size << 10)) + ps2.split_phys;
    return mem_read_ram(addr, priv);
}
static uint16_t
ps2_read_split_ramw(uint32_t addr, void *priv)
{
    addr = (addr % (ps2.split_size << 10)) + ps2.split_phys;
    return mem_read_ramw(addr, priv);
}
static uint32_t
ps2_read_split_raml(uint32_t addr, void *priv)
{
    addr = (addr % (ps2.split_size << 10)) + ps2.split_phys;
    return mem_read_raml(addr, priv);
}
static void
ps2_write_split_ram(uint32_t addr, uint8_t val, void *priv)
{
    addr = (addr % (ps2.split_size << 10)) + ps2.split_phys;
    mem_write_ram(addr, val, priv);
}
static void
ps2_write_split_ramw(uint32_t addr, uint16_t val, void *priv)
{
    addr = (addr % (ps2.split_size << 10)) + ps2.split_phys;
    mem_write_ramw(addr, val, priv);
}
static void
ps2_write_split_raml(uint32_t addr, uint32_t val, void *priv)
{
    addr = (addr % (ps2.split_size << 10)) + ps2.split_phys;
    mem_write_raml(addr, val, priv);
}

#define PS2_SETUP_IO      0x80
#define PS2_SETUP_VGA     0x20

#define PS2_ADAPTER_SETUP 0x08

static uint8_t
model_50_read(uint16_t port)
{
    switch (port) {
        case 0x100:
            return ps2.planar_id & 0xff;
        case 0x101:
            return ps2.planar_id >> 8;
        case 0x102:
            return ps2.option[0];
        case 0x103:
            return ps2.option[1];
        case 0x104:
            return ps2.option[2];
        case 0x105:
            return ps2.option[3];
        case 0x106:
            return ps2.subaddr_lo;
        case 0x107:
            return ps2.subaddr_hi;

        default:
            break;
    }
    return 0xff;
}

static uint8_t
model_55sx_read(uint16_t port)
{
    switch (port) {
        case 0x100:
            return ps2.planar_id & 0xff;
        case 0x101:
            return ps2.planar_id >> 8;
        case 0x102:
            return ps2.option[0];
        case 0x103:
            return ps2.option[1];
        case 0x104:
            return ps2.memory_bank[ps2.option[3] & 7];
        case 0x105:
            return ps2.option[3];
        case 0x106:
            return ps2.subaddr_lo;
        case 0x107:
            return ps2.subaddr_hi;

        default:
            break;
    }
    return 0xff;
}

static uint8_t
model_70_type3_read(uint16_t port)
{
    switch (port) {
        case 0x100:
            return ps2.planar_id & 0xff;
        case 0x101:
            return ps2.planar_id >> 8;
        case 0x102:
            return ps2.option[0];
        case 0x103:
            return ps2.option[1];
        case 0x104:
            return ps2.option[2];
        case 0x105:
            return ps2.option[3];
        case 0x106:
            return ps2.subaddr_lo;
        case 0x107:
            return ps2.subaddr_hi;

        default:
            break;
    }
    return 0xff;
}

static uint8_t
model_80_read(uint16_t port)
{
    switch (port) {
        case 0x100:
            return ps2.planar_id & 0xff;
        case 0x101:
            return ps2.planar_id >> 8;
        case 0x102:
            return ps2.option[0];
        case 0x103:
            return ps2.option[1];
        case 0x104:
            return ps2.option[2];
        case 0x105:
            return ps2.option[3];
        case 0x106:
            return ps2.subaddr_lo;
        case 0x107:
            return ps2.subaddr_hi;

        default:
            break;
    }
    return 0xff;
}

static uint8_t
ps55_model_50t_read(uint16_t port)
{
    ps2_mca_log(" Read SysBrd %04X xx %04X:%04X\n", port, cs >> 4, cpu_state.pc);
    switch (port) {
        case 0x100:
            return ps2.planar_id & 0xff;
        case 0x101:
            return ps2.planar_id >> 8;
        case 0x102:
            return ps2.option[0];
        case 0x103:
            {
                uint8_t val = 0xff;
                /*
                I/O 103h - Bit 7-4: Memory Card ID (Connector 1 or 3)
                           Bit 3-0: Memory Card ID (Connector 2)

                Memory Card ID: 7h = 2 MB Memory Card 2 or 3 Installed
                                5h = 4 MB Memory Card 2 Installed
                */
                switch (mem_size / 1024) {
                    case 2:
                        if (ps2.option[1] & 0x04)
                            val = 0xff;
                        else
                            val = 0x7f;
                        break;
                    case 4:
                        if (ps2.option[1] & 0x04)
                            val = 0xff;
                        else
                            val = 0x77;
                        break;
                    case 6:
                        if (ps2.option[1] & 0x04)
                            val = 0x7f;
                        else
                            val = 0x77;
                        break;
                    case 8:
                    default:
                        if (ps2.option[1] & 0x04)
                            val = 0x5f;
                        else
                            val = 0x77;
                        break;
                }
                ps2_mca_log(" Read MCA %04X %02X %04X:%04X mem_size = %d, ps2option1 = %2X\n", port, val, cs >> 4, cpu_state.pc, mem_size, ps2.option[1]);
                return val;
            }
        case 0x104:
            return ps2.option[2];
        case 0x105:
            return ps2.option[3];
        case 0x106:
            return ps2.subaddr_lo;
        case 0x107:
            return ps2.subaddr_hi;
    }
    return 0xff;
}

static uint8_t
ps55_model_50v_read(uint16_t port)
{
    switch (port) {
        case 0x100:
            return ps2.planar_id & 0xff;
        case 0x101:
            return ps2.planar_id >> 8;
        case 0x102:
            return ps2.option[0];
        case 0x103:
            {
                uint8_t val = 0xff;
                /*
                I/O 103h - Bit 7-4: Reserved
                           Bit 3-0: Memory Card ID (Connector 3 or 1)

                Memory Card ID: 8h = 4 MB Memory Card IV Installed
                                Fh = No Card Installed
                */
                switch (mem_size / 1024) {
                    case 4:
                        if (ps2.option[1] & 0x04)
                            val = 0xff;
                        else
                            val = 0xf8;
                        break;
                    case 8:
                    default:
                        if (ps2.option[1] & 0x04)
                            val = 0xf8;
                        else
                            val = 0xf8;
                        break;
                }
                return val;
            }
        case 0x104:
            /* Reading cache ID (bit 3-2) always returns zero */
            return ps2.option[2] & 0xf3;
        case 0x105:
            return ps2.option[3];
        case 0x106:
            return ps2.subaddr_lo;
        case 0x107:
            return ps2.subaddr_hi;
    }
    return 0xff;
}
static void
model_50_write(uint16_t port, uint8_t val)
{
    switch (port) {
        case 0x102:
            lpt_port_remove(ps2.lpt);
            serial_remove(ps2.uart);
            if (val & 0x04) {
                if (val & 0x08)
                    serial_setup(ps2.uart, COM1_ADDR, COM1_IRQ);
                else
                    serial_setup(ps2.uart, COM2_ADDR, COM2_IRQ);
            }
            if (val & 0x10) {
                switch ((val >> 5) & 3) {
                    case 0:
                        lpt_port_setup(ps2.lpt, LPT_MDA_ADDR);
                        break;
                    case 1:
                        lpt_port_setup(ps2.lpt, LPT1_ADDR);
                        break;
                    case 2:
                        lpt_port_setup(ps2.lpt, LPT2_ADDR);
                        break;

                    default:
                        break;
                }
            }
            ps2.option[0] = val;
            break;
        case 0x103:
            ps2.option[1] = (ps2.option[1] & 0xfe) | (val & 0x01);
            break;
        case 0x104:
            ps2.option[2] = val;
            break;
        case 0x105:
            ps2.option[3] = val;
            break;
        case 0x106:
            ps2.subaddr_lo = val;
            break;
        case 0x107:
            ps2.subaddr_hi = val;
            break;

        default:
            break;
    }
}

/*
 * On the 9552, planar POS byte 1 is fixed at BFh, but port 103h has a
 * second function while system setup port 94h is 7Fh: bit 0 selects one of
 * two memory-presence rows and the remaining bits report the optional IC
 * DRAM-card connectors.  Treating it as writable POS storage returned
 * BFh/BEh, neither of which is a valid 9552 memory signature.  The 4 MiB
 * base planar is reported elsewhere; with both optional connectors empty,
 * both selected rows read FFh.  At the supported 16 MiB maximum, the
 * machine has 12 MiB of IC DRAM expansion (one 4 MiB and one 8 MiB card).
 * The 9552 presence table encodes that two-card topology as 94h/84h.  Both
 * values are among the signatures accepted by the 39G9996 ROM; returning
 * BFh/BEh or FEh instead describes an unsupported module and raises 00225.
 */
static uint8_t
thinkpad_700_planar_read(uint16_t port)
{
    if ((port == 0x0103) && (ps2.setup == 0x7f)) {
        if (mem_size == 16384)
            return ps2.tp700_mem_presence_select ? 0x94 : 0x84;
        return 0xff;
    }

    return model_50_read(port);
}

static void
thinkpad_700_planar_write(uint16_t port, uint8_t val)
{
    if ((port == 0x0103) && (ps2.setup == 0x7f)) {
        ps2.tp700_mem_presence_select = val & 1;
        return;
    }

    model_50_write(port, val);
}

static void
model_55sx_mem_recalc(void)
{
    int state;
#ifdef ENABLE_PS2_MCA_LOG
    int enabled_mem = 0;
#endif
    int base             = 0;
    int remap_size       = (ps2.option[3] & 0x10) ? 384 : 256;
    int bit_mask         = 0x00;
    int max_rows         = 4;
    int bank_to_rows[16] = { 4, 2, 1, 0, 0, 2, 1, 0, 0, 0, 0, 0, 0, 2, 1, 0 };

    ps2_mca_log("%02X %02X\n", ps2.option[1], ps2.option[3]);

    mem_remap_top(remap_size);
    mem_set_mem_state(0x00000000, (mem_size + 384) * 1024, MEM_READ_EXTERNAL | MEM_WRITE_EXTERNAL);
    mem_set_mem_state(0x000e0000, 0x00020000, MEM_READ_EXTANY | MEM_WRITE_DISABLED);

    for (uint8_t i = 0; i < 2; i++) {
        max_rows = bank_to_rows[(ps2.memory_bank[i] >> 4) & 0x0f];

        if (max_rows == 0)
            continue;

        for (int j = 0; j < max_rows; j++) {
            if (ps2.memory_bank[i] & (1 << j)) {
                ps2_mca_log("Set memory at %06X-%06X to internal\n", (base * 1024), (base * 1024) + (((base > 0) ? 1024 : 640) * 1024) - 1);
                mem_set_mem_state(base * 1024, ((base > 0) ? 1024 : 640) * 1024, MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
#ifdef ENABLE_PS2_MCA_LOG
                enabled_mem += 1024;
#endif
                bit_mask |= (1 << (j + (i << 2)));
            }
            base += 1024;
        }
    }

#ifdef ENABLE_PS2_MCA_LOG
    ps2_mca_log("Enabled memory: %i kB (%02X)\n", enabled_mem, bit_mask);
#endif

    if (ps2.option[3] & 0x10) {
        /* Enable ROM. */
        ps2_mca_log("Enable ROM\n");
        state = MEM_READ_EXTANY;
    } else {
        /* Disable ROM. */
        if ((ps2.option[1] & 1) && !(ps2.option[3] & 0x20) && (bit_mask & 0x01)) {
            /* Disable RAM between 640 kB and 1 MB. */
            ps2_mca_log("Disable ROM, enable RAM\n");
            state = MEM_READ_INTERNAL;
        } else {
            ps2_mca_log("Disable ROM, disable RAM\n");
            state = MEM_READ_DISABLED;
        }
    }

    /* Write always disabled. */
    state |= MEM_WRITE_DISABLED;

    mem_set_mem_state(0xe0000, 0x20000, state);

#if 0
    if (!(ps2.option[3] & 0x08)) {
            ps2_mca_log("Memory not yet configured\n");
            return;
    }
#endif

    ps2_mca_log("Enable shadow mapping at %06X-%06X\n", (mem_size * 1024), (mem_size * 1024) + (remap_size * 1024) - 1);

    if ((ps2.option[1] & 1) && !(ps2.option[3] & 0x20) && (bit_mask & 0x01)) {
        ps2_mca_log("Set memory at %06X-%06X to internal\n", (mem_size * 1024), (mem_size * 1024) + (remap_size * 1024) - 1);
        mem_set_mem_state(mem_size * 1024, remap_size * 1024, MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
    }

    flushmmucache_nopc();
}

static void
model_55sx_write(uint16_t port, uint8_t val)
{
    switch (port) {
        case 0x102:
            lpt_port_remove(ps2.lpt);
            serial_remove(ps2.uart);
            if (val & 0x04) {
                if (val & 0x08)
                    serial_setup(ps2.uart, COM1_ADDR, COM1_IRQ);
                else
                    serial_setup(ps2.uart, COM2_ADDR, COM2_IRQ);
            }
            if (val & 0x10) {
                switch ((val >> 5) & 3) {
                    case 0:
                        lpt_port_setup(ps2.lpt, LPT_MDA_ADDR);
                        break;
                    case 1:
                        lpt_port_setup(ps2.lpt, LPT1_ADDR);
                        break;
                    case 2:
                        lpt_port_setup(ps2.lpt, LPT2_ADDR);
                        break;

                    default:
                        break;
                }
            }
            ps2.option[0] = val;
            break;
        case 0x103:
            ps2_mca_log("Write POS1: %02X\n", val);
            ps2.option[1] = val;
            model_55sx_mem_recalc();
            break;
        case 0x104:
            ps2.memory_bank[ps2.option[3] & 7] &= ~0xf;
            ps2.memory_bank[ps2.option[3] & 7] |= (val & 0xf);
            ps2_mca_log("Write memory bank %i: %02X\n", ps2.option[3] & 7, val);
            model_55sx_mem_recalc();
            break;
        case 0x105:
            ps2_mca_log("Write POS3: %02X\n", val);
            ps2.option[3] = val;
            model_55sx_mem_recalc();
            break;
        case 0x106:
            ps2.subaddr_lo = val;
            break;
        case 0x107:
            ps2.subaddr_hi = val;
            break;

        default:
            break;
    }
}

static void
model_70_type3_write(uint16_t port, uint8_t val)
{
    switch (port) {
        case 0x102:
            lpt_port_remove(ps2.lpt);
            serial_remove(ps2.uart);
            if (val & 0x04) {
                if (val & 0x08)
                    serial_setup(ps2.uart, COM1_ADDR, COM1_IRQ);
                else
                    serial_setup(ps2.uart, COM2_ADDR, COM2_IRQ);
            }
            if (val & 0x10) {
                switch ((val >> 5) & 3) {
                    case 0:
                        lpt_port_setup(ps2.lpt, LPT_MDA_ADDR);
                        break;
                    case 1:
                        lpt_port_setup(ps2.lpt, LPT1_ADDR);
                        break;
                    case 2:
                        lpt_port_setup(ps2.lpt, LPT2_ADDR);
                        break;

                    default:
                        break;
                }
            }
            ps2.option[0] = val;
            break;
        case 0x103:
            if (ps2.planar_id == 0xfff9)
                ps2.option[1] = (ps2.option[1] & 0x0f) | (val & 0xf0);
            break;
        case 0x104:
            if (ps2.planar_id == 0xfff9)
                ps2.option[2] = val;
            break;
        case 0x105:
            ps2.option[3] = val;
            break;
        case 0x106:
            ps2.subaddr_lo = val;
            break;
        case 0x107:
            ps2.subaddr_hi = val;
            break;

        default:
            break;
    }
}

static void
model_80_write(uint16_t port, uint8_t val)
{
    switch (port) {
        case 0x102:
            lpt_port_remove(ps2.lpt);
            serial_remove(ps2.uart);
            if (val & 0x04) {
                if (val & 0x08)
                    serial_setup(ps2.uart, COM1_ADDR, COM1_IRQ);
                else
                    serial_setup(ps2.uart, COM2_ADDR, COM2_IRQ);
            }
            if (val & 0x10) {
                switch ((val >> 5) & 3) {
                    case 0:
                        lpt_port_setup(ps2.lpt, LPT_MDA_ADDR);
                        break;
                    case 1:
                        lpt_port_setup(ps2.lpt, LPT1_ADDR);
                        break;
                    case 2:
                        lpt_port_setup(ps2.lpt, LPT2_ADDR);
                        break;

                    default:
                        break;
                }
            }
            ps2.option[0] = val;
            break;
        case 0x103:
            ps2.option[1] = (ps2.option[1] & 0x0f) | (val & 0xf0);
            break;
        case 0x104:
            ps2.option[2] = val;
            break;
        case 0x105:
            ps2.option[3] = val;
            break;
        case 0x106:
            ps2.subaddr_lo = val;
            break;
        case 0x107:
            ps2.subaddr_hi = val;
            break;

        default:
            break;
    }
}

static void
ps55_model_50tv_write(uint16_t port, uint8_t val)
{
    ps2_mca_log(" Write SysBrd %04X %02X %04X:%04X\n", port, val, cs >> 4, cpu_state.pc);
    switch (port) {
        case 0x102:
            lpt_port_remove(ps2.lpt);
            serial_remove(ps2.uart);
            if (val & 0x04) {
                if (val & 0x08)
                    serial_setup(ps2.uart, COM1_ADDR, COM1_IRQ);
                else
                    serial_setup(ps2.uart, COM2_ADDR, COM2_IRQ);
            }
            if (val & 0x10) {
                switch ((val >> 5) & 3) {
                    case 0:
                        lpt_port_setup(ps2.lpt, LPT_MDA_ADDR);
                        break;
                    case 1:
                        lpt_port_setup(ps2.lpt, LPT1_ADDR);
                        break;
                    case 2:
                        lpt_port_setup(ps2.lpt, LPT2_ADDR);
                        break;
                    default:
                        break;
                }
            }
            ps2.option[0] = val;
            break;
        case 0x103:
            ps2.option[1] = val;
            break;
        case 0x104:
            if ((ps2.option[2] ^ val) & 1) {
                /* Disable/Enable E0000 - E0FFF (Make 2 KB hole for Display Adapter) */
                ps2.option[2] = val;
                mem_encoding_update();
            }
            ps2.option[2] = val;
            break;
        case 0x105:
            ps2.option[3] = val;
            break;
        case 0x106:
            ps2.subaddr_lo = val;
            break;
        case 0x107:
            ps2.subaddr_hi = val;
            break;
        default:
            break;
    }
}

uint8_t
ps2_mca_read(const uint16_t port, UNUSED(void *priv))
{
    uint8_t temp;

    switch (port) {
        case 0x91:
#if 0
            fatal("Read 91 setup=%02x adapter=%02x\n", ps2.setup, ps2.adapter_setup);
#endif
            if (!(ps2.setup & PS2_SETUP_IO))
                temp = 0x00;
            else if (!(ps2.setup & PS2_SETUP_VGA))
                temp = 0x00;
            else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                temp = 0x00;
            else
                temp = !mca_feedb();
            temp |= 0xfe;
            break;
        case 0x94:
            temp = ps2.setup;
            break;
        case 0x96:
            temp = ps2.adapter_setup | 0x70;
            break;
        case 0x100:
            if (!(ps2.setup & PS2_SETUP_IO))
                temp = ps2.planar_read(port);
            else if (!(ps2.setup & PS2_SETUP_VGA))
                temp = 0xfd;
            else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                temp = mca_read(port);
            else
                temp = 0xff;
            break;
        case 0x101:
            if (!(ps2.setup & PS2_SETUP_IO))
                temp = ps2.planar_read(port);
            else if (!(ps2.setup & PS2_SETUP_VGA))
                temp = 0xef;
            else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                temp = mca_read(port);
            else
                temp = 0xff;
            break;
        case 0x102:
            if (!(ps2.setup & PS2_SETUP_IO))
                temp = ps2.planar_read(port);
            else if (!(ps2.setup & PS2_SETUP_VGA))
                temp = ps2.pos_vga;
            else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                temp = mca_read(port);
            else
                temp = 0xff;
            break;
        case 0x103:
            if (!(ps2.setup & PS2_SETUP_IO))
                temp = ps2.planar_read(port);
            else if ((ps2.setup & PS2_SETUP_VGA) && (ps2.adapter_setup & PS2_ADAPTER_SETUP))
                temp = mca_read(port);
            else
                temp = 0xff;
            break;
        case 0x104:
            if (!(ps2.setup & PS2_SETUP_IO))
                temp = ps2.planar_read(port);
            else if ((ps2.setup & PS2_SETUP_VGA) && (ps2.adapter_setup & PS2_ADAPTER_SETUP))
                temp = mca_read(port);
            else
                temp = 0xff;
            break;
        case 0x105:
            if (!(ps2.setup & PS2_SETUP_IO))
                temp = ps2.planar_read(port);
            else if ((ps2.setup & PS2_SETUP_VGA) && (ps2.adapter_setup & PS2_ADAPTER_SETUP))
                temp = mca_read(port);
            else
                temp = 0xff;
            break;
        case 0x106:
            if (!(ps2.setup & PS2_SETUP_IO))
                temp = ps2.planar_read(port);
            else if ((ps2.setup & PS2_SETUP_VGA) && (ps2.adapter_setup & PS2_ADAPTER_SETUP))
                temp = mca_read(port);
            else
                temp = 0xff;
            break;
        case 0x107:
            if (!(ps2.setup & PS2_SETUP_IO))
                temp = ps2.planar_read(port);
            else if ((ps2.setup & PS2_SETUP_VGA) && (ps2.adapter_setup & PS2_ADAPTER_SETUP))
                temp = mca_read(port);
            else
                temp = 0xff;
            break;

        default:
            temp = 0xff;
            break;
    }

    ps2_mca_log("ps2_read: port=%04x temp=%02x\n", port, temp);

    return temp;
}

static void
ps2_mca_write(const uint16_t port, uint8_t val, UNUSED(void *priv))
{
    ps2_mca_log("ps2_write: port=%04x val=%02x %04x:%04x\n", port, val, CS, cpu_state.pc);

    switch (port) {
        case 0x94:
            ps2.setup = val;
            break;
        case 0x96:
            if ((val & 0x80) && !(ps2.adapter_setup & 0x80)) {
                mca_reset();
                if (ps2.has_e0000_hole) {
                    /* Reset memstate for E0000 - E0FFFh hole (for PS/55 5550-V)
                       5550-T does this in POST, but 5550-V doesn't. */
                    ps2.option[2] &= 0xFE;
                    mem_encoding_update();
                }
            }
            ps2.adapter_setup = val;
            mca_set_index(val & 7);
            break;
        case 0x100:
            if (!(ps2.setup & PS2_SETUP_IO))
                ps2.planar_write(port, val);
            else if ((ps2.setup & PS2_SETUP_VGA) && (ps2.adapter_setup & PS2_ADAPTER_SETUP))
                mca_write(port, val);
            break;
        case 0x101:
            if (!(ps2.setup & PS2_SETUP_IO))
                ps2.planar_write(port, val);
            else if ((ps2.setup & PS2_SETUP_VGA) && (ps2.adapter_setup & PS2_ADAPTER_SETUP))
                mca_write(port, val);
            break;
        case 0x102:
            if (!(ps2.setup & PS2_SETUP_IO))
                ps2.planar_write(port, val);
            else if (!(ps2.setup & PS2_SETUP_VGA)) {
                if (ps2.mb_vga) {
                    if (vga_isenabled(ps2.mb_vga))
                        vga_disable(ps2.mb_vga);
                    if (val & 1)
                        vga_enable(ps2.mb_vga);
                }
                ps2.pos_vga = val;
            } else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                mca_write(port, val);
            break;
        case 0x103:
            if (!(ps2.setup & PS2_SETUP_IO))
                ps2.planar_write(port, val);
            else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                mca_write(port, val);
            break;
        case 0x104:
            if (!(ps2.setup & PS2_SETUP_IO))
                ps2.planar_write(port, val);
            else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                mca_write(port, val);
            break;
        case 0x105:
            if (!(ps2.setup & PS2_SETUP_IO))
                ps2.planar_write(port, val);
            else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                mca_write(port, val);
            break;
        case 0x106:
            if (!(ps2.setup & PS2_SETUP_IO))
                ps2.planar_write(port, val);
            else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                mca_write(port, val);
            break;
        case 0x107:
            if (!(ps2.setup & PS2_SETUP_IO))
                ps2.planar_write(port, val);
            else if (ps2.adapter_setup & PS2_ADAPTER_SETUP)
                mca_write(port, val);
            break;

        default:
            break;
    }
}

static void
ps2_mca_board_common_init(void)
{
    io_sethandler(0x0091, 0x0001, ps2_mca_read, NULL, NULL, ps2_mca_write, NULL, NULL, NULL);
    io_sethandler(0x0094, 0x0001, ps2_mca_read, NULL, NULL, ps2_mca_write, NULL, NULL, NULL);
    io_sethandler(0x0096, 0x0001, ps2_mca_read, NULL, NULL, ps2_mca_write, NULL, NULL, NULL);
    io_sethandler(0x0100, 0x0008, ps2_mca_read, NULL, NULL, ps2_mca_write, NULL, NULL, NULL);

    device_add(&port_6x_ps2_device);
    device_add(&port_92_device);

    ps2.setup = 0xff;

    lpt_port_setup(ps2.lpt, LPT_MDA_ADDR);
}

static uint8_t
ps2_mem_expansion_read(const uint16_t port, UNUSED(void *priv))
{
    return ps2.mem_pos_regs[port & 7];
}

static void
ps2_mem_expansion_write(const uint16_t port, uint8_t val, UNUSED(void *priv))
{
    if (port < 0x102 || port == 0x104)
        return;

    ps2.mem_pos_regs[port & 7] = val;

    if (ps2.mem_pos_regs[2] & 1)
        mem_mapping_enable(&ps2.expansion_mapping);
    else
        mem_mapping_disable(&ps2.expansion_mapping);
}

static uint8_t
ps2_mem_expansion_feedb(UNUSED(void *priv))
{
    return (ps2.mem_pos_regs[2] & 1);
}

static void
ps2_mca_mem_fffc_init(int start_mb)
{
    uint32_t planar_size;
    uint32_t expansion_start;

    planar_size     = (start_mb - 1) << 20;
    expansion_start = start_mb << 20;

    mem_mapping_set_addr(&ram_high_mapping, 0x100000, planar_size);

    ps2.mem_pos_regs[0] = 0xff;
    ps2.mem_pos_regs[1] = 0xfc;

    switch ((mem_size / 1024) - start_mb) {
        case 1:
            ps2.mem_pos_regs[4] = 0xfc; /* 11 11 11 00 = 0 0 0 1 */
            break;
        case 2:
            ps2.mem_pos_regs[4] = 0xfe; /* 11 11 11 10 = 0 0 0 2 */
            break;
        case 3:
            ps2.mem_pos_regs[4] = 0xf2; /* 11 11 00 10 = 0 0 1 2 */
            break;
        case 4:
            ps2.mem_pos_regs[4] = 0xfa; /* 11 11 10 10 = 0 0 2 2 */
            break;
        case 5:
            ps2.mem_pos_regs[4] = 0xca; /* 11 00 10 10 = 0 1 2 2 */
            break;
        case 6:
            ps2.mem_pos_regs[4] = 0xea; /* 11 10 10 10 = 0 2 2 2 */
            break;
        case 7:
            ps2.mem_pos_regs[4] = 0x2a; /* 00 10 10 10 = 1 2 2 2 */
            break;
        case 8:
            ps2.mem_pos_regs[4] = 0xaa; /* 10 10 10 10 = 2 2 2 2 */
            break;

        default:
            break;
    }

    mca_add(ps2_mem_expansion_read, ps2_mem_expansion_write, ps2_mem_expansion_feedb, NULL, NULL);
    mem_mapping_add(&ps2.expansion_mapping,
                    expansion_start,
                    (mem_size - (start_mb << 10)) << 10,
                    mem_read_ram,
                    mem_read_ramw,
                    mem_read_raml,
                    mem_write_ram,
                    mem_write_ramw,
                    mem_write_raml,
                    &ram[expansion_start],
                    MEM_MAPPING_INTERNAL,
                    NULL);
    mem_mapping_disable(&ps2.expansion_mapping);
}

static void
ps2_mca_mem_d071_init(int start_mb)
{
    uint32_t planar_size;
    uint32_t expansion_start;

    planar_size     = (start_mb - 1) << 20;
    expansion_start = start_mb << 20;

    mem_mapping_set_addr(&ram_high_mapping, 0x100000, planar_size);

    ps2.mem_pos_regs[0] = 0xd0;
    ps2.mem_pos_regs[1] = 0x71;
    ps2.mem_pos_regs[4] = (mem_size / 1024) - start_mb;

    mca_add(ps2_mem_expansion_read, ps2_mem_expansion_write, ps2_mem_expansion_feedb, NULL, NULL);
    mem_mapping_add(&ps2.expansion_mapping,
                    expansion_start,
                    (mem_size - (start_mb << 10)) << 10,
                    mem_read_ram,
                    mem_read_ramw,
                    mem_read_raml,
                    mem_write_ram,
                    mem_write_ramw,
                    mem_write_raml,
                    &ram[expansion_start],
                    MEM_MAPPING_INTERNAL,
                    NULL);
    mem_mapping_disable(&ps2.expansion_mapping);
}

static void
ps2_mca_board_model_50_init(void)
{
    ps2_mca_board_common_init();

    mem_remap_top(384);
    mca_init(4);

    ps2.planar_read  = model_50_read;
    ps2.planar_write = model_50_write;

    /*
     I/O 103h - Bit 3: Memory Presence Detect 2
                Bit 2: Memory Presence Detect 1
                Bit 1: Password Override Jumper
                Bit 0: Enable System Board RAM
    */

    switch (mem_size / 1024) {
        case 0: /*256Kx2*/
            ps2.option[1] = 0xf8;
            break;
        case 1: /*512Kx2*/
            ps2.option[1] = 0xf4;
            break;
        case 2: /*1Mx2*/
        default:
            ps2.option[1] = 0xf0;
            break;
    }

    /* Enable password function */
    ps2.option[1] |= 0x02;

    if (mem_size > 2048) {
        /*
         * Keep the Model 50 memory controller active for now: the 9552 ROM
         * uses its POS-programmed aperture during POST.  The ThinkPad 700
         * Reference Diskette will be taught that FFFCh is onboard memory so
         * it no longer treats this implementation detail as a missing card.
         */
        ps2_mca_mem_fffc_init(2);
    }

    if (gfxcard[0] == VID_INTERNAL)
        device_add(&ps1vga_mca_device);
}

/*
 * The ThinkPad 700 is not a portable Model 50.  In particular, all of its
 * standard 4 MB is base-planar memory.  Reusing the Model 50 initialiser used
 * to expose only 2 MB on the planar and synthesize the other 2 MB as an FFFC
 * MCA expansion card.  The 9552 POST quite correctly reported that topology
 * as a changed memory configuration (00164) and, before the card was enabled,
 * as a memory-size failure (00201).
 *
 * PF1EE.ADF describes the 9552 planar as a two-slot system and gives the
 * fixed POS bits below.  Keep the complete configured RAM address space on
 * the planar mapping, while port 103h reports the real 4+8 MiB optional-card
 * topology used for the supported 16 MiB configuration.
 */
static void
ps2_mca_board_thinkpad_700_init(void)
{
    ps2_mca_board_common_init();

    /*
     * The 9552 planar reserves the upper 192 KiB of the conventional 384 KiB
     * adapter/firmware hole.  Only A0000h-CFFFFh is backed by relocatable
     * planar DRAM; D0000h-FFFFFh is firmware/control storage.  Relocating the
     * full 384 KiB makes POST count 3456 KiB above base memory (0D80h), while
     * the machine configuration correctly records 3264 KiB (0CC0h).  That
     * real topology mismatch is what raises 00164 on every cold start.
     */
    mem_remap_top(192);
    mca_init(2);

    ps2.planar_read  = thinkpad_700_planar_read;
    ps2.planar_write = thinkpad_700_planar_write;

    /* PF1EE.ADF fixed planar POS values (POS 2 through POS 5). */
    ps2.option[0] = 0x03;
    ps2.option[1] = 0xbf;
    ps2.option[2] = 0x02;
    ps2.option[3] = 0x10;
    ps2.tp700_mem_presence_select = 1;

    if (gfxcard[0] == VID_INTERNAL)
        device_add(&ps1vga_mca_device);
}

static void
ps2_mca_board_model_60_init(void)
{
    ps2_mca_board_common_init();

    mem_remap_top(384);
    mca_init(8);

    ps2.planar_read  = model_50_read;
    ps2.planar_write = model_50_write;

    /*
     I/O 103h - Bit 3: Memory Presence Detect 2 (Reversed)
                Bit 2: Memory Presence Detect 1 (Reversed)
                Bit 1: Password Override Jumper
                Bit 0: Enable System Board RAM
    */

    switch (mem_size / 1024) {
        case 0: /*256Kx2*/
            ps2.option[1] = 0xf0;
            break;       
        case 1: /*256Kx4*/
            ps2.option[1] = 0xf4;
            break;
        case 2: /*512Kx4*/
        case 3: /*Not supported*/
            ps2.option[1] = 0xf8;
            break;
        case 4: /*1Mx4*/
        default:
            ps2.option[1] = 0xfc;
            break;
    }
    
    /* Enable password function */
    ps2.option[1] |= 0x02;

    if (mem_size > 4096) {
        /* Only 4 MB supported on planar, create a memory expansion card for the rest */
        ps2_mca_mem_fffc_init(4);
    }

    device_add(&ps2_nvr_55ls_device);

    if (gfxcard[0] == VID_INTERNAL)
        device_add(&ps1vga_mca_device);
}

static void
ps2_mca_board_model_55sx_init(int has_sec_nvram, int slots)
{
    ps2_mca_board_common_init();

    ps2.option[1] = 0x00;
    ps2.option[2] = 0x00;
    ps2.option[3] = 0x10;

    memset(ps2.memory_bank, 0xf0, 8);
    switch (mem_size / 1024) {
        case 1:
            ps2.memory_bank[0] = 0x61;
            break;
        case 2:
            ps2.memory_bank[0] = 0x51;
            break;
        case 3:
            ps2.memory_bank[0] = 0x51;
            ps2.memory_bank[1] = 0x61;
            break;
        case 4:
            ps2.memory_bank[0] = 0x51;
            ps2.memory_bank[1] = 0x51;
            break;
        case 5:
            ps2.memory_bank[0] = 0x01;
            ps2.memory_bank[1] = 0x61;
            break;
        case 6:
        case 7: /*Not supported*/
            ps2.memory_bank[0] = 0x01;
            ps2.memory_bank[1] = 0x51;
            break;
        case 8:
            ps2.memory_bank[0] = 0x01;
            ps2.memory_bank[1] = 0x01;
            break;

        default:
            break;
    }

    mca_init(slots);

    if (has_sec_nvram)
        device_add(&ps2_nvr_55ls_device);

    ps2.planar_read  = model_55sx_read;
    ps2.planar_write = model_55sx_write;

    if (gfxcard[0] == VID_INTERNAL)
        device_add(&ps1vga_mca_device);

    model_55sx_mem_recalc();
}

static void
mem_encoding_update(void)
{
    mem_mapping_disable(&ps2.split_mapping);

    if (ps2.split_size > 0)
        mem_set_mem_state(ps2.split_addr, ps2.split_size << 10, MEM_READ_EXTANY | MEM_WRITE_EXTANY);
    if (((mem_size << 10) - (1 << 20)) > 0)
        mem_set_mem_state(1 << 20, (mem_size << 10) - (1 << 20), MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);

    ps2.split_addr = ((uint32_t) (ps2.mem_regs[0] & 0xf)) << 20;
    if (!ps2.split_addr)
        ps2.split_addr = 1 << 20;

    if (ps2.mem_regs[1] & 2) {
        mem_set_mem_state(0xe0000, 0x20000, MEM_READ_EXTANY | MEM_WRITE_INTERNAL);
        ps2_mca_log("PS/2 Model 80-111: ROM space enabled\n");
    } else {
        mem_set_mem_state(0xe0000, 0x20000, MEM_READ_INTERNAL | MEM_WRITE_DISABLED);
        ps2_mca_log("PS/2 Model 80-111: ROM space disabled\n");
    }

    if (ps2.mem_regs[1] & 4) {
        mem_mapping_set_addr(&ram_low_mapping, 0x00000, 0x80000);
        ps2_mca_log("PS/2 Model 80-111: 00080000- 0009FFFF disabled\n");
    } else {
        mem_mapping_set_addr(&ram_low_mapping, 0x00000, 0xa0000);
        ps2_mca_log("PS/2 Model 80-111: 00080000- 0009FFFF enabled\n");
    }

    if (!(ps2.mem_regs[1] & 8)) {
        if (ps2.mem_regs[1] & 4) {
            ps2.split_size = 384;
            ps2.split_phys = 0x80000;
        } else {
            ps2.split_size = 256;
            ps2.split_phys = 0xa0000;
        }

        mem_set_mem_state(ps2.split_addr, ps2.split_size << 10, MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
        mem_mapping_set_exec(&ps2.split_mapping, &ram[ps2.split_phys]);
        mem_mapping_set_addr(&ps2.split_mapping, ps2.split_addr, ps2.split_size << 10);

        ps2_mca_log("PS/2 Model 80-111: Split memory block enabled at %08X\n", ps2.split_addr);
    } else {
        ps2.split_size = 0;
        ps2_mca_log("PS/2 Model 80-111: Split memory block disabled\n");
    }

    if (ps2.has_e0000_hole && (ps2.option[2] & 1)) {
        /* Set memstate for E0000 - E0FFFh hole (PS/55 only) */
        mem_set_mem_state(0xe0000, 0x1000, MEM_READ_EXTERNAL | MEM_WRITE_EXTERNAL);
    }

    flushmmucache_nopc();
}

static uint8_t
mem_encoding_read(uint16_t addr, UNUSED(void *priv))
{
    switch (addr) {
        case 0xe0:
            return ps2.mem_regs[0];
        case 0xe1:
            return ps2.mem_regs[1];

        default:
            break;
    }
    return 0xff;
}
static void
mem_encoding_write(uint16_t addr, uint8_t val, UNUSED(void *priv))
{
    switch (addr) {
        case 0xe0:
            ps2.mem_regs[0] = val;
            break;
        case 0xe1:
            ps2.mem_regs[1] = val;
            break;

        default:
            break;
    }
    mem_encoding_update();
}

static uint8_t
mem_encoding_read_cached(uint16_t addr, UNUSED(void *priv))
{
    switch (addr) {
        case 0xe0:
            return ps2.mem_regs[0];
        case 0xe1:
            return ps2.mem_regs[1];
        case 0xe2:
            return ps2.mem_regs[2];

        default:
            break;
    }
    return 0xff;
}

static void
mem_encoding_write_cached(uint16_t addr, uint8_t val, UNUSED(void *priv))
{
    uint8_t old;

    switch (addr) {
        case 0xe0:
            ps2.mem_regs[0] = val;
            break;
        case 0xe1:
            ps2.mem_regs[1] = val;
            break;
        case 0xe2:
            old             = ps2.mem_regs[2];
            ps2.mem_regs[2] = (ps2.mem_regs[2] & 0x80) | (val & ~0x88);
            if (val & 2) {
                ps2_mca_log("Clear latch - %i\n", ps2.pending_cache_miss);
                if (ps2.pending_cache_miss)
                    ps2.mem_regs[2] |= 0x80;
                else
                    ps2.mem_regs[2] &= ~0x80;
                ps2.pending_cache_miss = 0;
            }

            if ((val & 0x21) == 0x20 && (old & 0x21) != 0x20)
                ps2.pending_cache_miss = 1;
            if ((val & 0x21) == 0x01 && (old & 0x21) != 0x01)
                ps2_cache_clean();
#if 1
            // FIXME: Look into this!!!
            if (val & 0x01)
                ram_mid_mapping.flags |= MEM_MAPPING_ROM_WS;
            else
                ram_mid_mapping.flags &= ~MEM_MAPPING_ROM_WS;
#endif
            break;

        default:
            break;
    }
    ps2_mca_log("mem_encoding_write: addr=%02x val=%02x %04x:%04x  %02x %02x\n", addr, val, CS, cpu_state.pc, ps2.mem_regs[1], ps2.mem_regs[2]);
    mem_encoding_update();
    if ((ps2.mem_regs[1] & 0x10) && (ps2.mem_regs[2] & 0x21) == 0x20) {
        mem_mapping_disable(&ram_low_mapping);
        mem_mapping_enable(&ps2.cache_mapping);
        flushmmucache();
    } else {
        mem_mapping_disable(&ps2.cache_mapping);
        mem_mapping_enable(&ram_low_mapping);
        flushmmucache();
    }
}

static void
ps2_mca_board_model_70_type34_init(int is_type4, int slots)
{
    ps2_mca_board_common_init();

    ps2.split_addr = mem_size * 1024;
    mca_init(slots);

    ps2.planar_read  = model_70_type3_read;
    ps2.planar_write = model_70_type3_write;

    device_add(&ps2_nvr_device);

    io_sethandler(0x00e0, 0x0003, mem_encoding_read_cached, NULL, NULL, mem_encoding_write_cached, NULL, NULL, NULL);

    ps2.mem_regs[1] = 2;

    switch (mem_size / 1024) {
        case 2:
            ps2.option[1] = 0xa6;
            ps2.option[2] = 0x01;
            break;
        case 4:
            ps2.option[1] = 0xaa;
            ps2.option[2] = 0x01;
            break;
        case 6:
            ps2.option[1] = 0xca;
            ps2.option[2] = 0x01;
            break;
        case 8:
        default:
            ps2.option[1] = 0xca;
            ps2.option[2] = 0x02;
            break;
    }

    if (is_type4)
        ps2.option[2] |= 0x04; /*486 CPU*/

    mem_mapping_add(&ps2.split_mapping,
                    (mem_size + 256) * 1024,
                    256 * 1024,
                    ps2_read_split_ram,
                    ps2_read_split_ramw,
                    ps2_read_split_raml,
                    ps2_write_split_ram,
                    ps2_write_split_ramw,
                    ps2_write_split_raml,
                    &ram[0xa0000],
                    MEM_MAPPING_INTERNAL,
                    NULL);
    mem_mapping_disable(&ps2.split_mapping);

    mem_mapping_add(&ps2.cache_mapping,
                    0,
                    is_type4 ? (8 * 1024) : (64 * 1024),
                    ps2_read_cache_ram,
                    ps2_read_cache_ramw,
                    ps2_read_cache_raml,
                    ps2_write_cache_ram,
                    NULL,
                    NULL,
                    ps2_cache,
                    MEM_MAPPING_INTERNAL,
                    NULL);
    mem_mapping_disable(&ps2.cache_mapping);

    if (ps2.planar_id == 0xfff9) {
        if (mem_size > 4096) {
            /* Only 4 MB supported on planar, create a memory expansion card for the rest */
            if (mem_size > 12288) {
                ps2_mca_mem_d071_init(4);
            } else {
                ps2_mca_mem_fffc_init(4);
            }
        }
    } else {
        if (mem_size > 8192) {
            /* Only 8 MB supported on planar, create a memory expansion card for the rest */
            if (mem_size > 16384)
                ps2_mca_mem_d071_init(8);
            else {
                ps2_mca_mem_fffc_init(8);
            }
        }
    }

    if (gfxcard[0] == VID_INTERNAL)
        ps2.mb_vga = device_add(&ps1vga_mca_device);
}

static void
ps2_mca_board_model_80_type2_init(void)
{
    ps2_mca_board_common_init();

    ps2.split_addr = mem_size * 1024;
    mca_init(8);

    ps2.planar_read  = model_80_read;
    ps2.planar_write = model_80_write;

    device_add(&ps2_nvr_device);

    io_sethandler(0x00e0, 0x0002, mem_encoding_read, NULL, NULL, mem_encoding_write, NULL, NULL, NULL);

    ps2.mem_regs[1] = 2;

    /* Note by Kotori: I rewrote this because the original code was using
       Model 80 Type 1-style 1 MB memory card settings, which are *NOT*
       supported by Model 80 Type 2. */
    switch (mem_size / 1024) {
        case 1:
            ps2.option[1]   = 0x0e; /* 11 10 = 0 2 */
            ps2.mem_regs[1] = 0xd2; /* 01 = 1 (first) */
            ps2.mem_regs[0] = 0xf0; /* 11 = invalid */
            break;
        case 2:
            ps2.option[1]   = 0x0e; /* 11 10 = 0 2 */
            ps2.mem_regs[1] = 0xc2; /* 00 = 2 */
            ps2.mem_regs[0] = 0xf0; /* 11 = invalid */
            break;
        case 3:
            ps2.option[1]   = 0x0a; /* 10 10 = 2 2 */
            ps2.mem_regs[1] = 0xc2; /* 00 = 2 */
            ps2.mem_regs[0] = 0xd0; /* 01 = 1 (first) */
            break;
        case 4:
        default:
            ps2.option[1]   = 0x0a; /* 10 10 = 2 2 */
            ps2.mem_regs[1] = 0xc2; /* 00 = 2 */
            ps2.mem_regs[0] = 0xc0; /* 00 = 2 */
            break;
    }

    ps2.mem_regs[0] |= ((mem_size / 1024) & 0x0f);

    mem_mapping_add(&ps2.split_mapping,
                    (mem_size + 256) * 1024,
                    256 * 1024,
                    ps2_read_split_ram,
                    ps2_read_split_ramw,
                    ps2_read_split_raml,
                    ps2_write_split_ram,
                    ps2_write_split_ramw,
                    ps2_write_split_raml,
                    &ram[0xa0000],
                    MEM_MAPPING_INTERNAL,
                    NULL);
    mem_mapping_disable(&ps2.split_mapping);

    if (mem_size > 4096) {
        /* Only 4 MB supported on planar, create a memory expansion card for the rest */
        if (mem_size > 12288)
            ps2_mca_mem_d071_init(4);
        else {
            ps2_mca_mem_fffc_init(4);
        }
    }

    if (gfxcard[0] == VID_INTERNAL)
        ps2.mb_vga = device_add(&ps1vga_mca_device);

    ps2.split_size = 0;
}

static void
machine_ps2_common_init(const machine_t *model)
{
    machine_common_init(model);

    if (fdc_current[0] == FDC_INTERNAL) {
        const device_t *fdc_device = (model->device == &thinkpad_700_device)
                                   ? &fdc_ps2_mca_powerdown_device
                                   : &fdc_ps2_mca_device;
        device_add(fdc_device);
    }

    dma16_init();
    ps2_dma_init();
    pic2_init();

    int pit_type = ((pit_mode == -1 && is486) || pit_mode == 1) ? PIT_8254_FAST : PIT_8254;
    pit_ps2_init(pit_type);

    nmi_mask = 0x80;

    ps2.uart = device_add_inst(&ns16550_device, 1);

    ps2.lpt = device_add_inst(&lpt_port_device, 1);
    lpt_set_ext(ps2.lpt, 1);

    ps2.has_e0000_hole = 0;
}

static const device_config_t ps2_model_50_config[] = {
    // clang-format off
    {
        .name           = "bios",
        .description    = "BIOS Version",
        .type           = CONFIG_BIOS,
        .default_string = "ibmps2_m50",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = {
            {
                .name          = "IBM PS/2 model 50",
                .internal_name = "ibmps2_m50",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 4,
                .local         = 0,
                .size          = 131072,
                .files         = { "roms/machines/ibmps2_m50/90x7420.zm13", "roms/machines/ibmps2_m50/90x7429.zm18",
                                   "roms/machines/ibmps2_m50/90x7423.zm14", "roms/machines/ibmps2_m50/90x7426.zm16", "" }
            },
            {
                .name          = "IBM PS/2 model 50Z",
                .internal_name = "ibmps2_m50z",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 2,
                .local         = 0,
                .size          = 131072,
                .files         = { "roms/machines/ibmps2_m50/15F8366.BIN", "roms/machines/ibmps2_m50/15F8365.BIN", "" }
            },
            { .files_no = 0 }
        }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t ps2_model_50_device = {
    .name          = "IBM PS/2 model 50",
    .internal_name = "ibmps2_m50",
    .flags         = 0,
    .local         = 0,
    .init          = NULL,
    .close         = NULL,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = ps2_model_50_config
};

/*
 * The only currently preserved 39G9996 dump is the 700C/F1EC build.  IBM's
 * monochrome 700 Reference Diskette supplies PF1EE.ADF and the ROM contains
 * the same planar ID at three mirrored operands in each 128 KiB half.  The
 * physical EPROM part number was shared; select the monochrome build for this
 * machine by converting those operands in the loaded copy and repairing the
 * two affected 64 KiB checksums.  The user's source dump remains untouched.
 */
static int
thinkpad_700_select_monochrome_rom(void)
{
    static const uint32_t planar_id_offsets[] = {
        0x1057b, 0x107f2, 0x11be1, 0x30378, 0x305ee, 0x31517
    };
    int converted = 0;

    for (unsigned i = 0; i < sizeof(planar_id_offsets) / sizeof(planar_id_offsets[0]); i++) {
        const uint32_t offset = planar_id_offsets[i];

        if ((rom[offset] == 0xec) && (rom[offset + 1] == 0xf1)) {
            rom[offset] = 0xee;
            converted++;
        } else if (!((rom[offset] == 0xee) && (rom[offset + 1] == 0xf1))) {
            pclog("TP700 ROM: unexpected planar-ID operand at %05X: %02X%02X\n",
                  offset, rom[offset], rom[offset + 1]);
            return 0;
        }
    }

    if (converted) {
        if (converted != 6) {
            pclog("TP700 ROM: mixed 700/700C planar-ID operands\n");
            return 0;
        }
        if ((rom[0x1ffff] != 0x16) || (rom[0x3ffff] != 0x39)) {
            pclog("TP700 ROM: unexpected 700C checksum bytes %02X/%02X\n",
                  rom[0x1ffff], rom[0x3ffff]);
            return 0;
        }
        rom[0x1ffff] = 0x10;
        rom[0x3ffff] = 0x33;
    }

    for (unsigned bank = 0; bank < 4; bank++) {
        uint8_t sum = 0;

        for (unsigned i = 0; i < 0x10000; i++)
            sum += rom[(bank << 16) + i];
        if (sum) {
            pclog("TP700 ROM: bank %u checksum failed (%02X)\n", bank, sum);
            return 0;
        }
    }

    pclog("TP700 ROM: selected checksum-correct monochrome F1EE build\n");
    return 1;
}

static const device_config_t thinkpad_700_config[] = {
    // clang-format off
    {
        .name           = "bios",
        .description    = "BIOS Version",
        .type           = CONFIG_BIOS,
        .default_string = "ibmtp700_39g9996",
        .default_int    = 0,
        .file_filter    = NULL,
        .spinner        = { 0 },
        .selection      = { { 0 } },
        .bios           = {
            {
                .name          = "IBM ThinkPad 700 39G9996",
                .internal_name = "ibmtp700_39g9996",
                .bios_type     = BIOS_NORMAL,
                .files_no      = 1,
                .local         = 0,
                .size          = 262144,
                .files         = { "roms/machines/ibmtp700/39G9996.bin", "" }
            },
            { .files_no = 0 }
        }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t thinkpad_700_device = {
    .name          = "IBM ThinkPad 700 (9552)",
    .internal_name = "ibmtp700",
    .flags         = 0,
    .local         = 0,
    .init          = NULL,
    .close         = NULL,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = thinkpad_700_config
};

int
machine_thinkpad_700_init(const machine_t *model)
{
    int         ret;
    const char *fn;
    uint16_t    vector;

    if (!device_available(model->device))
        return 0;

    device_context(model->device);
    fn  = device_get_bios_file(machine_get_device(machine), device_get_config_bios("bios"), 0);
    ret = bios_load_linear(fn, 0x000c0000, 262144, 0);
    device_context_restore();

    pclog("TP700 INIT: BIOS=%s loaded=%d bios_only=%d\n", fn, ret, bios_only);

    if (ret && !thinkpad_700_select_monochrome_rom())
        return 0;

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    /*
     * The 486SLC exposes only the upper 128 KiB of the 9552 firmware at the
     * top of its 24-bit address space.  The generic 256 KiB BIOS loader maps
     * the complete image at FC0000h-FFFFFFh; that incorrectly hides an extra
     * 128 KiB of installed RAM.  It is invisible with the factory 4 MiB but
     * at the supported 16 MiB maximum POST then measures 3B00h KiB of
     * extended RAM while Automatic Configuration records the correct 3B80h,
     * causing 00164 after every restart.  Keep the complete low shadow at
     * C0000h-FFFFFh, but expose only the reset/POST half at FE0000h-FFFFFFh.
     */
    mem_mapping_set_addr(&bios_high_mapping, 0x00fe0000, 0x00020000);
    mem_mapping_set_exec(&bios_high_mapping, rom + 0x00020000);
    mem_set_mem_state_both(0x00fc0000, 0x00020000,
                           MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
    mem_set_mem_state_both(0x00fe0000, 0x00020000,
                           MEM_READ_ROMCS | MEM_WRITE_ROMCS);
    flushmmucache();

    /* The Model 50 auxiliary PIT output is wired to NMI; the 9552 is not. */
    nmi_mask = 0;

    /* The monochrome ThinkPad 700 planar is F1EE (F1EC is the 700C). */
    ps2.planar_id = 0xf1ee;
    ps2_mca_board_thinkpad_700_init();
    device_add(&ps2_nvr_device);

    /*
     * The firmware uses the top of its ROM window as writable runtime
     * storage (for example the control block at FA40:0660).  Use the 9552's
     * distinct shadow store so the relocated A0000h-FFFFFh DRAM remains
     * independently testable above 4 MiB.
     */
    memcpy(thinkpad_700_shadow_ram, rom, sizeof(thinkpad_700_shadow_ram));
    mem_mapping_add(&thinkpad_700_shadow_mapping,
                    0x000c0000, sizeof(thinkpad_700_shadow_ram),
                    thinkpad_700_shadow_read,
                    thinkpad_700_shadow_readw,
                    thinkpad_700_shadow_readl,
                    thinkpad_700_shadow_write,
                    thinkpad_700_shadow_writew,
                    thinkpad_700_shadow_writel,
                    thinkpad_700_shadow_ram,
                    MEM_MAPPING_INTERNAL, NULL);
    pclog("TP700 SHADOW INIT: ROM[37800]=%02X %02X %02X %02X "
          "SHADOW[37800]=%02X %02X %02X %02X\n",
          rom[0x37800], rom[0x37801], rom[0x37802], rom[0x37803],
          thinkpad_700_shadow_ram[0x37800], thinkpad_700_shadow_ram[0x37801],
          thinkpad_700_shadow_ram[0x37802], thinkpad_700_shadow_ram[0x37803]);
    mem_set_mem_state_both(0x000c0000, 0x00040000,
                           MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
    flushmmucache();

    memset(ps2.tp700_sysctl_regs, 0, sizeof(ps2.tp700_sysctl_regs));
    ps2.tp700_sysctl_index = 0;
    io_sethandler(0x00e0, 0x0001,
                  thinkpad_700_sysctl_read, NULL, NULL,
                  thinkpad_700_sysctl_write, NULL, NULL, NULL);
    io_sethandler(0x00e3, 0x0001,
                  thinkpad_700_sysctl_read, NULL, NULL,
                  thinkpad_700_sysctl_write, NULL, NULL, NULL);
    io_sethandler(0x00e4, 0x0001,
                  thinkpad_700_sysctl_read, NULL, NULL,
                  thinkpad_700_sysctl_write, NULL, NULL, NULL);

    memset(ps2.tp700_sm_regs, 0, sizeof(ps2.tp700_sm_regs));
    memset(ps2.tp700_sm_packet, 0, sizeof(ps2.tp700_sm_packet));
    memset(ps2.tp700_sm_parameters, 0, sizeof(ps2.tp700_sm_parameters));
    ps2.tp700_sm_index  = 0;
    ps2.tp700_sm_status = 0;
    ps2.tp700_sm_command = 0;
    ps2.tp700_sm_packet_pos = 0;
    io_sethandler(0x00b0, 0x0003,
                  thinkpad_700_sm_read, NULL, NULL,
                  thinkpad_700_sm_write, NULL, NULL, NULL);

    /*
     * 39G9996 checks this planar block after its first successful disk read.
     * Port 79h contains the serial-storage status (zero means present/ready),
     * while 7Ch is its latched control register.  An unclaimed I/O port reads
     * as FFh and makes the BIOS deliberately halt with POST error 0186.
     */
    memset(ps2.tp700_ss_regs, 0, sizeof(ps2.tp700_ss_regs));
    memset(ps2.tp700_ss_words, 0, sizeof(ps2.tp700_ss_words));
    /*
     * The 9552 serial EEPROM is split into two CRC-protected VPD regions.
     * A blank, factory-serviceable EEPROM is not all zeroes: word 7 protects
     * words 8..40, and word 0 protects words 1..6 plus 41..63.  The IBM IML
     * uses the CRC-16 routine at 0000:4293 with an FFFFh seed and expects a
     * zero remainder when the stored checksum is appended.  Supplying the
     * correct blank-region checksums models an unprogrammed board instead of
     * a corrupt one; the Reference Diskette may then populate identification
     * data through the normal EEPROM write protocol.
     */
    ps2.tp700_ss_words[0] = 0xc53e;
    ps2.tp700_ss_words[7] = 0xd5b6;
    /* Words 29h..30h are the 16-byte system-unit serial number.  The EEPROM
       service presents each word low byte first. */
    {
        static const char tp700_serial[16] = "9552-700-0000001";

        for (unsigned i = 0; i < 8; i++)
            ps2.tp700_ss_words[0x29 + i] =
                (uint8_t) tp700_serial[i * 2] |
                ((uint16_t) (uint8_t) tp700_serial[i * 2 + 1] << 8);
    }
    ps2.tp700_ss_command = 0;
    io_sethandler(0x0078, 0x0005,
                  thinkpad_700_ss_read, NULL, NULL,
                  thinkpad_700_ss_write, NULL, NULL, NULL);

    /*
     * The 9552 stage-1 ROM calls INT 16h while doing its very early keyboard
     * setup, before the generic PS/2 path has populated the IVT.  Its hardware
     * and standard INT 08h..1Fh entry table starts at F000:FEF3.  Populate
     * those vectors from the ROM's own table rather than emulating any BIOS
     * service here.  In particular, INT 0Eh is the diskette IRQ6 handler.
     */
    for (vector = 0x08; vector <= 0x1f; vector++) {
        const uint32_t table = 0x3fef3 + ((vector - 0x08) << 1);
        const uint16_t entry = rom[table] | (rom[table + 1] << 8);
        const uint32_t ivt   = vector << 2;

        ram[ivt]     = entry & 0xff;
        ram[ivt + 1] = entry >> 8;
        ram[ivt + 2] = 0x00;
        ram[ivt + 3] = entry ? 0xf0 : 0x00;
    }

    /*
     * The same ROM initialization routine installs the slave-PIC vectors
     * 70h..77h from the offset table at F000:FF23.  The early 9552 path does
     * not run that generic initializer before using INT 15h/AH=86h, whose
     * RTC completion interrupt is IRQ8/vector 70h.  Without these entries
     * the interrupt follows uninitialized low RAM and eventually executes
     * zero-filled ROM space.  Vector 70h is F000:FF33 in 39G9996 and jumps
     * to the BIOS RTC handler at F000:E98C.
     */
    for (vector = 0x70; vector <= 0x77; vector++) {
        const uint32_t table = 0x3ff23 + ((vector - 0x70) << 1);
        const uint16_t entry = rom[table] | (rom[table + 1] << 8);
        const uint32_t ivt   = vector << 2;

        ram[ivt]     = entry & 0xff;
        ram[ivt + 1] = entry >> 8;
        ram[ivt + 2] = 0x00;
        ram[ivt + 3] = entry ? 0xf0 : 0x00;
    }

    /*
     * The 9552 reset path consults the EBDA before its first POST checkpoint.
     * With the Model 50 base, BDA 40:0Eh is otherwise still zero at that
     * point, so the ROM's EBDA status update at offset 68h aliases IVT 1Ah
     * and changes its entry from FE6Eh to FE6Fh.  Establish the ThinkPad's
     * 1 KiB EBDA up front, as the real planar does, so those early accesses
     * land in the intended control area.
     */
    memset(&ram[0x0009fc00], 0, 0x400);
    ram[0x0009fc00]            = 1;
    *(uint16_t *) &ram[0x040e] = 0x9fc0;
    *(uint16_t *) &ram[0x0413] = 0x027f;

    pclog("TP700 INIT: planar=%04X RAM=%dKB EBDA=9FC0 "
          "reference disk may now be read\n",
          ps2.planar_id, mem_size);

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

int
machine_ps2_model_50_init(const machine_t *model)
{
    int         ret = 0;
    const char *fn[4];

    /* No ROMs available */
    if (!device_available(model->device))
        return ret;

    device_context(model->device);
    int is_50z = !strcmp(device_get_config_bios("bios"), "ibmps2_m50z");
    if (is_50z) {
        for (uint8_t i = 0; i < 2; i++)
            fn[i] = device_get_bios_file(machine_get_device(machine), device_get_config_bios("bios"), i);
        ret = bios_load_interleaved(fn[0], fn[1], 0x000e0000, 131072, 0);
    } else {
        for (uint8_t i = 0; i < 4; i++)
            fn[i] = device_get_bios_file(machine_get_device(machine), device_get_config_bios("bios"), i);
        ret = bios_load_interleaved(fn[0], fn[1], 0x000f0000, 131072, 0);
        ret &= bios_load_aux_interleaved(fn[2], fn[3], 0x000e0000, 65536, 0);
    }
    device_context_restore();

    machine_ps2_common_init(model);

    ps2.planar_id = 0xfbff;
    ps2_mca_board_model_50_init();

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

int
machine_ps2_model_60_init(const machine_t *model)
{
    int ret;

    ret = bios_load_interleaved("roms/machines/ibmps2_m50/90x7420.zm13",
                                "roms/machines/ibmps2_m50/90x7429.zm18",
                                0x000f0000, 131072, 0);
    ret &= bios_load_aux_interleaved("roms/machines/ibmps2_m50/90x7423.zm14",
                                     "roms/machines/ibmps2_m50/90x7426.zm16",
                                     0x000e0000, 65536, 0);

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    ps2.planar_id = 0xf7ff;
    ps2_mca_board_model_60_init();

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

int
machine_ps2_model_55sx_init(const machine_t *model)
{
    int ret;

    ret = bios_load_interleaved("roms/machines/ibmps2_m55sx/33f8146.zm41",
                                "roms/machines/ibmps2_m55sx/33f8145.zm40",
                                0x000e0000, 131072, 0);

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    ps2.planar_id = 0xfbff;
    ps2_mca_board_model_55sx_init(0, 4);

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

int
machine_ps2_model_65sx_init(const machine_t *model)
{
    int ret;

    ret = bios_load_interleaved("roms/machines/ibmps2_m65sx/64F3608.BIN",
                                "roms/machines/ibmps2_m65sx/64F3611.BIN",
                                0x000e0000, 131072, 0);

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    ps2.planar_id = 0xe3ff;
    ps2_mca_board_model_55sx_init(1, 8);

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

int
machine_ps2_model_70_type3_init(const machine_t *model)
{
    int ret;

    ret = bios_load_interleaved("roms/machines/ibmps2_m70_type3/70-a_even.bin",
                                "roms/machines/ibmps2_m70_type3/70-a_odd.bin",
                                0x000e0000, 131072, 0);

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    ps2.planar_id = 0xf9ff;
    ps2_mca_board_model_70_type34_init(0, 4);

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

int
machine_ps2_model_80_init(const machine_t *model)
{
    int ret;

    ret = bios_load_interleaved("roms/machines/ibmps2_m80/15f6637.bin",
                                "roms/machines/ibmps2_m80/15f6639.bin",
                                0x000e0000, 131072, 0);

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    ps2.planar_id = 0xfdff;
    ps2_mca_board_model_80_type2_init();

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

int
machine_ps2_model_80_axx_init(const machine_t *model)
{
    int ret;

    ret = bios_load_interleaved("roms/machines/ibmps2_m80/64f4356.bin",
                                "roms/machines/ibmps2_m80/64f4355.bin",
                                0x000e0000, 131072, 0);

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    ps2.planar_id = 0xfff9;
    ps2_mca_board_model_70_type34_init(0, 8);

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

int
machine_ps2_model_70_type4_init(const machine_t *model)
{
    int ret;

    ret = bios_load_interleaved("roms/machines/ibmps2_m70_type4/64F3126.BIN",
                                "roms/machines/ibmps2_m70_type4/64F3125.BIN",
                                0x000e0000, 131072, 0);

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    ps2.planar_id = 0xf9ff;
    ps2_mca_board_model_70_type34_init(1, 4);

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

static void
ps55_mca_board_model_50t_init(void)
{
    ps2_mca_board_common_init();

    ps2.split_addr = mem_size * 1024;
    /* The slot 5 is reserved for the Integrated Fixed Disk II (an internal ESDI hard drive). */
    mca_init(5);

    ps2.planar_read  = ps55_model_50t_read;
    ps2.planar_write = ps55_model_50tv_write;

    device_add(&ps2_nvr_device);

    io_sethandler(0x00e0, 0x0002, mem_encoding_read, NULL, NULL, mem_encoding_write, NULL, NULL, NULL);

    ps2.mem_regs[1] = 2;
    ps2.option[2] &= 0xfe; /* Bit 0: Disable E0000-E0FFFh (4 KB) */
    ps2.has_e0000_hole = 1;

    mem_mapping_add(&ps2.split_mapping,
                    (mem_size + 256) * 1024,
                    256 * 1024,
                    ps2_read_split_ram,
                    ps2_read_split_ramw,
                    ps2_read_split_raml,
                    ps2_write_split_ram,
                    ps2_write_split_ramw,
                    ps2_write_split_raml,
                    &ram[0xa0000],
                    MEM_MAPPING_INTERNAL,
                    NULL);
    mem_mapping_disable(&ps2.split_mapping);

    if (mem_size > 8192) {
        /* Only 8 MB supported on planar, create a memory expansion card for the rest */
        ps2_mca_mem_fffc_init(8);
    }

    if (gfxcard[0] == VID_INTERNAL)
        ps2.mb_vga = (vga_t *) device_add(&ps1vga_mca_device);
}

static void
ps55_mca_board_model_50v_init(void)
{
    ps2_mca_board_common_init();

    ps2.split_addr = mem_size * 1024;
    /* The slot 5 is reserved for the Integrated Fixed Disk II (an internal ESDI hard drive). */
    mca_init(5);

    ps2.planar_read  = ps55_model_50v_read;
    ps2.planar_write = ps55_model_50tv_write;

    device_add(&ps2_nvr_device);

    io_sethandler(0x00e0, 0x0003, mem_encoding_read_cached, NULL, NULL, mem_encoding_write_cached, NULL, NULL, NULL);

    ps2.mem_regs[1] = 2;
    ps2.option[2] &= 0xf2; /*   Bit 3-2: -Cache IDs, Bit 1: Reserved
                                Bit 0: Disable E0000-E0FFFh (4 KB) */
    ps2.has_e0000_hole = 1;

    mem_mapping_add(&ps2.split_mapping,
                    (mem_size + 256) * 1024,
                    256 * 1024,
                    ps2_read_split_ram,
                    ps2_read_split_ramw,
                    ps2_read_split_raml,
                    ps2_write_split_ram,
                    ps2_write_split_ramw,
                    ps2_write_split_raml,
                    &ram[0xa0000],
                    MEM_MAPPING_INTERNAL,
                    NULL);
    mem_mapping_disable(&ps2.split_mapping);

    mem_mapping_add(&ps2.cache_mapping,
                    0,
                    64 * 1024,
                    ps2_read_cache_ram,
                    ps2_read_cache_ramw,
                    ps2_read_cache_raml,
                    ps2_write_cache_ram,
                    NULL,
                    NULL,
                    ps2_cache,
                    MEM_MAPPING_INTERNAL,
                    NULL);
    mem_mapping_disable(&ps2.cache_mapping);

    /* Only 8 MB supported on planar, create a memory expansion card for the rest */
    if (mem_size > 8192)
        ps2_mca_mem_fffc_init(8);

    if (gfxcard[0] == VID_INTERNAL)
        ps2.mb_vga = (vga_t *) device_add(&ps1vga_mca_device);
}

int
machine_ps55_model_50t_init(const machine_t *model)
{
    int ret;

    ret = bios_load_linear("roms/machines/ibmps55_m50t/38F6933.BIN",
                           0x000e0000, 131072, 0);

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    /*
     * Planar ID
     * FFFAh - PS/55 model 5551-S0x, T0x (stage 1?)
     * FFEEh - PS/55 model 5551-S1x, T1x (stage 2?)
     * Verification in BIOS P/N 38F6933: FBxx -> 4 slots (error), xxEE -> 5 slots (ok), others -> 8 (error)
     *
     * The only difference between S and T models is the CPU speed (16 MHz vs 20 MHz).
     * The POST measures the speed, and sets a flag in the BIOS Data Area to indicate the sub model.
     * The VM in 86Box runs faster than the real, so the POST always determines it as the T model.
     */
    ps2.planar_id = 0xffee;
    ps55_mca_board_model_50t_init();

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}

int
machine_ps55_model_50v_init(const machine_t *model)
{
    int ret;

    ret = bios_load_interleaved("roms/machines/ibmps55_m50v/56F7416.BIN",
                                "roms/machines/ibmps55_m50v/56F7417.BIN",
                                0x000e0000, 131072, 0);

    if (bios_only || !ret)
        return ret;

    machine_ps2_common_init(model);

    /*
     * Planar ID
     * F1FFh - PS/55 model 5551-V0x, V1x
     * Verification in BIOS P/N 56F7416,56F7417: FBxx -> 5 slots (ok), F1xx -> 5 slots (ok), others -> 8 (error)
     */
    ps2.planar_id = 0xf1ff;
    ps55_mca_board_model_50v_init();

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);

    return ret;
}
