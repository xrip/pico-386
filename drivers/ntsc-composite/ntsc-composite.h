#pragma once

#include <stdint.h>

// NTSC composite on GPIO22, sent by a PIO state machine at SYS 495 MHz.
// Call on the core that is to take the scanline IRQ (DMA_IRQ_0).
void ntsc_composite_init(void);
void ntsc_composite_set_palette(uint8_t index, uint32_t color888);
