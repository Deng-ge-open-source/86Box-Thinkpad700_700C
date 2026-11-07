#ifdef USE_NEW_DYNAREC
#    define CPU_SET_OXPC
#else
#    define CPU_SET_OXPC oxpc = cpu_state.pc;
#endif

#define RETF_a16(stack_offset)                        \
    if ((msw & 1) && !(cpu_state.eflags & VM_FLAG)) { \
        op_pmoderetf(0, stack_offset);                \
        return 1;                                     \
    }                                                 \
    CPU_SET_OXPC                                      \
    if (stack32) {                                    \
        cpu_state.pc = readmemw(ss, ESP);             \
        op_loadcs(readmemw(ss, ESP + 2));             \
    } else {                                          \
        cpu_state.pc = readmemw(ss, SP);              \
        op_loadcs(readmemw(ss, (SP + 2) & 0xffff));   \
    }                                                 \
    if (cpu_state.abrt)                               \
        return 1;                                     \
    if (stack32)                                      \
        ESP += 4 + stack_offset;                      \
    else                                              \
        SP += 4 + stack_offset;                       \
    cycles -= timing_retf_rm;

#define RETF_a32(stack_offset)                        \
    if ((msw & 1) && !(cpu_state.eflags & VM_FLAG)) { \
        op_pmoderetf(1, stack_offset);                \
        return 1;                                     \
    }                                                 \
    CPU_SET_OXPC                                      \
    if (stack32) {                                    \
        cpu_state.pc = readmeml(ss, ESP);             \
        op_loadcs(readmeml(ss, ESP + 4) & 0xffff);    \
    } else {                                          \
        cpu_state.pc = readmeml(ss, SP);              \
        op_loadcs(readmeml(ss, SP + 4) & 0xffff);     \
    }                                                 \
    if (cpu_state.abrt)                               \
        return 1;                                     \
    if (stack32)                                      \
        ESP += 8 + stack_offset;                      \
    else                                              \
        SP += 8 + stack_offset;                       \
    cycles -= timing_retf_rm;

static int
opRETF_a16(UNUSED(uint32_t fetchdat))
{
    int cycles_old = cycles;
    UN_USED(cycles_old);

    CPU_BLOCK_END();
    RETF_a16(0);

    PREFETCH_RUN(cycles_old - cycles, 1, -1, 2, 0, 0, 0, 0);
    PREFETCH_FLUSH();
    return 0;
}
static int
opRETF_a32(UNUSED(uint32_t fetchdat))
{
    int cycles_old = cycles;
    UN_USED(cycles_old);

    CPU_BLOCK_END();
    RETF_a32(0);

    PREFETCH_RUN(cycles_old - cycles, 1, -1, 0, 2, 0, 0, 1);
    PREFETCH_FLUSH();
    return 0;
}

static int
opRETF_a16_imm(uint32_t fetchdat)
{
    uint16_t offset     = getwordf();
    int      cycles_old = cycles;
    UN_USED(cycles_old);

    if (0 && !strcmp(machine_get_internal_name(), "ibmtp700") &&
        (CS == 0xf000) && (cpu_state.oldpc == 0x6d14) &&
        ((AX == 0x9001) || (AX == 0x9101)))
        pclog("TP700 RETF2 BEFORE(2386): AX=%04X SS:SP=%04X:%04X "
              "use32=%03X stack32=%d words=%04X %04X %04X %04X\n",
              AX, SS, (uint16_t) ESP, use32, stack32,
              readmemw(ss, SP), readmemw(ss, (SP + 2) & 0xffff),
              readmemw(ss, (SP + 4) & 0xffff),
              readmemw(ss, (SP + 6) & 0xffff));

    CPU_BLOCK_END();
    RETF_a16(offset);

    if (0 && !strcmp(machine_get_internal_name(), "ibmtp700") &&
        ((AX == 0x9001) || (AX == 0x9101)))
        pclog("TP700 RETF2 AFTER(2386): AX=%04X CS:IP=%04X:%04X "
              "SS:SP=%04X:%04X\n", AX, CS, (uint16_t) cpu_state.pc,
              SS, (uint16_t) ESP);

    PREFETCH_RUN(cycles_old - cycles, 3, -1, 2, 0, 0, 0, 0);
    PREFETCH_FLUSH();
    return 0;
}
static int
opRETF_a32_imm(uint32_t fetchdat)
{
    uint16_t offset     = getwordf();
    int      cycles_old = cycles;
    UN_USED(cycles_old);

    CPU_BLOCK_END();
    RETF_a32(offset);

    PREFETCH_RUN(cycles_old - cycles, 3, -1, 0, 2, 0, 0, 1);
    PREFETCH_FLUSH();
    return 0;
}

static int
opIRET_186(UNUSED(uint32_t fetchdat))
{
    int cycles_old = cycles;
    UN_USED(cycles_old);

    if ((cr0 & 1) && (cpu_state.eflags & VM_FLAG) && (IOPL != 3)) {
        x86gpf(NULL, 0);
        return 1;
    }
    if (msw & 1) {
        optype = IRET;
        op_pmodeiret(0);
        optype = 0;
    } else {
        uint16_t new_cs;
        CPU_SET_OXPC
        if (stack32) {
            cpu_state.pc    = readmemw(ss, ESP);
            new_cs          = readmemw(ss, ESP + 2);
            cpu_state.flags = (cpu_state.flags & 0x7000) | (readmemw(ss, ESP + 4) & 0xffd5) | 2;
            ESP += 6;
        } else {
            cpu_state.pc    = readmemw(ss, SP);
            new_cs          = readmemw(ss, ((SP + 2) & 0xffff));
            cpu_state.flags = (cpu_state.flags & 0x7000) | (readmemw(ss, ((SP + 4) & 0xffff)) & 0x0fd5) | 2;
            SP += 6;
        }
        op_loadcs(new_cs);
        cycles -= timing_iret_rm;
    }
    flags_extract();
    nmi_enable = 1;
    rf_flag_no_clear = 1;
    CPU_BLOCK_END();

    PREFETCH_RUN(cycles_old - cycles, 1, -1, 2, 0, 0, 0, 0);
    PREFETCH_FLUSH();
    return cpu_state.abrt;
}

static int
opIRET_286(UNUSED(uint32_t fetchdat))
{
    int cycles_old = cycles;
    UN_USED(cycles_old);

    if ((cr0 & 1) && (cpu_state.eflags & VM_FLAG) && (IOPL != 3)) {
        x86gpf(NULL, 0);
        return 1;
    }
    if (msw & 1) {
        optype = IRET;
        op_pmodeiret(0);
        optype = 0;
    } else {
        uint16_t new_cs;
        CPU_SET_OXPC
        if (stack32) {
            cpu_state.pc    = readmemw(ss, ESP);
            new_cs          = readmemw(ss, ESP + 2);
            cpu_state.flags = (cpu_state.flags & 0x7000) | (readmemw(ss, ESP + 4) & 0xffd5) | 2;
            ESP += 6;
        } else {
            cpu_state.pc    = readmemw(ss, SP);
            new_cs          = readmemw(ss, ((SP + 2) & 0xffff));
            cpu_state.flags = (cpu_state.flags & 0x7000) | (readmemw(ss, ((SP + 4) & 0xffff)) & 0x0fd5) | 2;
            SP += 6;
        }
        op_loadcs(new_cs);
        cycles -= timing_iret_rm;
    }
    flags_extract();
    nmi_enable = 1;
    rf_flag_no_clear = 1;
    CPU_BLOCK_END();

    PREFETCH_RUN(cycles_old - cycles, 1, -1, 2, 0, 0, 0, 0);
    PREFETCH_FLUSH();
    return cpu_state.abrt;
}

static int
opIRET(UNUSED(uint32_t fetchdat))
{
    int cycles_old = cycles;
    UN_USED(cycles_old);
    const int tp700_fdc_iret = 0;

    if (tp700_fdc_iret)
        pclog("TP700 FDC IRET BEFORE(2386): SS:SP=%04X:%04X "
              "words=%04X %04X %04X | %04X %04X %04X %04X "
              "AX=%04X DS=%04X FLAGS=%04X DS3E=%02X\n", SS, (uint16_t) ESP,
              readmemw(ss, SP), readmemw(ss, (SP + 2) & 0xffff),
              readmemw(ss, (SP + 4) & 0xffff),
              readmemw(ss, (SP + 6) & 0xffff),
              readmemw(ss, (SP + 8) & 0xffff),
              readmemw(ss, (SP + 10) & 0xffff),
              readmemw(ss, (SP + 12) & 0xffff),
              AX, DS, cpu_state.flags, readmemb(ds, 0x003e));

    if ((cr0 & 1) && (cpu_state.eflags & VM_FLAG) && (IOPL != 3)) {
        if (cr4 & CR4_VME) {
            uint16_t new_pc;
            uint16_t new_cs;
            uint16_t new_flags;

            new_pc    = readmemw(ss, SP);
            new_cs    = readmemw(ss, ((SP + 2) & 0xffff));
            new_flags = readmemw(ss, ((SP + 4) & 0xffff));
            if (cpu_state.abrt)
                return 1;

            if ((new_flags & T_FLAG) || ((new_flags & I_FLAG) && (cpu_state.eflags & VIP_FLAG))) {
                x86gpf(NULL, 0);
                return 1;
            }
            SP += 6;
            if (new_flags & I_FLAG)
                cpu_state.eflags |= VIF_FLAG;
            else
                cpu_state.eflags &= ~VIF_FLAG;
            cpu_state.flags = (cpu_state.flags & 0x3300) | (new_flags & 0x4cd5) | 2;
            op_loadcs(new_cs);
            cpu_state.pc = new_pc;

            cycles -= timing_iret_rm;
        } else {
            x86gpf_expected(NULL, 0);
            return 1;
        }
    } else {
        if (msw & 1) {
            optype = IRET;
            op_pmodeiret(0);
            optype = 0;
        } else {
            uint16_t new_cs;
            CPU_SET_OXPC
            if (stack32) {
                cpu_state.pc    = readmemw(ss, ESP);
                new_cs          = readmemw(ss, ESP + 2);
                cpu_state.flags = (readmemw(ss, ESP + 4) & 0xffd5) | 2;
                ESP += 6;
            } else {
                cpu_state.pc    = readmemw(ss, SP);
                new_cs          = readmemw(ss, ((SP + 2) & 0xffff));
                cpu_state.flags = (readmemw(ss, ((SP + 4) & 0xffff)) & 0xffd5) | 2;
                SP += 6;
            }
            op_loadcs(new_cs);
            cycles -= timing_iret_rm;
        }
    }
    flags_extract();
    tp700_int21_trace_return(CS, (uint16_t) cpu_state.pc, AX,
                             (uint16_t) cpu_state.flags);
    if (tp700_fdc_iret)
        pclog("TP700 FDC IRET AFTER(2386): CS:IP=%04X:%04X SS:SP=%04X:%04X "
              "DS=%04X FLAGS=%04X DS3E=%02X\n", CS, (uint16_t) cpu_state.pc,
              SS, (uint16_t) ESP, DS, cpu_state.flags, readmemb(ds, 0x003e));
    nmi_enable = 1;
    rf_flag_no_clear = 1;
    CPU_BLOCK_END();

    PREFETCH_RUN(cycles_old - cycles, 1, -1, 2, 0, 0, 0, 0);
    PREFETCH_FLUSH();
    return cpu_state.abrt;
}

static int
opIRETD(UNUSED(uint32_t fetchdat))
{
    int cycles_old = cycles;
    UN_USED(cycles_old);

    if ((cr0 & 1) && (cpu_state.eflags & VM_FLAG) && (IOPL != 3)) {
        x86gpf_expected(NULL, 0);
        return 1;
    }
    if (msw & 1) {
        optype = IRET;
        op_pmodeiret(1);
        optype = 0;
    } else {
        uint16_t new_cs;
        CPU_SET_OXPC
        if (stack32) {
            cpu_state.pc     = readmeml(ss, ESP);
            new_cs           = readmemw(ss, ESP + 4);
            cpu_state.flags  = (readmemw(ss, ESP + 8) & 0xffd5) | 2;
            cpu_state.eflags = readmemw(ss, ESP + 10);
            ESP += 12;
        } else {
            cpu_state.pc     = readmeml(ss, SP);
            new_cs           = readmemw(ss, ((SP + 4) & 0xffff));
            cpu_state.flags  = (readmemw(ss, (SP + 8) & 0xffff) & 0xffd5) | 2;
            cpu_state.eflags = readmemw(ss, (SP + 10) & 0xffff);
            SP += 12;
        }
        op_loadcs(new_cs);
        cycles -= timing_iret_rm;
    }
    flags_extract();
    nmi_enable = 1;
    rf_flag_no_clear = 1;
    CPU_BLOCK_END();

    PREFETCH_RUN(cycles_old - cycles, 1, -1, 0, 2, 0, 0, 1);
    PREFETCH_FLUSH();
    return cpu_state.abrt;
}
