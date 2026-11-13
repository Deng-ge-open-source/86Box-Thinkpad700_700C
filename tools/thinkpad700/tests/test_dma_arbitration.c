/* Focused regression for MCA arbitration levels versus PS/2 DMA channels.
 * Build with GCC whole-program optimization and linker garbage collection; unused
 * emulator entry points are discarded so no ROMs or VM are required. */
#include <assert.h>
#include <stdio.h>
#include "../../../src/dma.c"

int
main(void)
{
    memset(dma, 0, sizeof(dma));
    dma_e = 0xff;
    dma_m = 0;
    dma_ps2.is_ps2 = 1;
    dma_ps2.arb_assigned = 0;

    assert(dma_ps2_get_channel(5) == 5);
    assert(dma_ps2_get_channel(14) == -1);
    assert(dma_ps2_get_channel(-1) == -1);

    /* An adapter selecting arbitration E must transfer on assigned channel 0. */
    dma[0].arb_level = 14;
    dma_ps2.arb_assigned = 1;
    assert(dma_ps2_get_channel(14) == 0);

    /* Prefer the enabled, unmasked assignment when firmware reassigns E. */
    dma_m = 1;
    dma[3].arb_level = 14;
    dma_ps2.arb_assigned |= 8;
    assert(dma_ps2_get_channel(14) == 3);
    dma_e &= ~8;
    assert(dma_ps2_get_channel(14) == 0);

    dma_ps2.is_ps2 = 0;
    assert(dma_ps2_get_channel(14) == -1);
    assert(dma_ps2_get_channel(5) == 5);
    puts("DMA arbitration regression passed: E, reassignment, masking, legacy fallback.");
    return 0;
}
