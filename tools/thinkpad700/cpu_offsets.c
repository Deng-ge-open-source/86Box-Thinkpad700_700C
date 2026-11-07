#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu/cpu.h"

int
main(void)
{
    printf("sizeof=%zu regs=%zu pc=%zu cycles=%zu cs=%zu ds=%zu es=%zu ss=%zu flags=%zu\n",
           sizeof(cpu_state_t), offsetof(cpu_state_t, regs), offsetof(cpu_state_t, pc),
           offsetof(cpu_state_t, _cycles), offsetof(cpu_state_t, seg_cs),
           offsetof(cpu_state_t, seg_ds), offsetof(cpu_state_t, seg_es),
           offsetof(cpu_state_t, seg_ss), offsetof(cpu_state_t, flags));
    return 0;
}
