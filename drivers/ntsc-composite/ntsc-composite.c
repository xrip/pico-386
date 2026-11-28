/*----------------------------------------------------------------------------
Copyright (C) 2026, xrip, all right reserved.
Copyright (C) 2024, KenKen, all right reserved.

This program supplied herewith by KenKen is free software; you can
redistribute it and/or modify it under the terms of the same license written
here and only for non-commercial purpose.

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of FITNESS FOR A PARTICULAR
PURPOSE. The copyright owner and contributors are NOT LIABLE for any damages
caused by using this program.

----------------------------------------------------------------------------*/

// NTSC composite from a PIO state machine, beside the HSTX output. Taken
// from rp2040-ntsc-tv-ntsc-pio, drivers/ntsc-composite (commit c447169).
// The signal generation (DMA scanlines of PWM-like pulses) is the idea of
// @lovyan03, https://github.com/lovyan03/

#pragma GCC optimize("O3")

#include "ntsc-composite.h"

#include <graphics_modes.h>

#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/irq.h>
#include <hardware/pio.h>

#define NTSC_PIN 22

// A sample is 11 bits at 157.5 Mbit/s; at 495 MHz that is 7 bits in 22 clocks.
#define NTSC_SAMPLE_BITS 11u
#define NTSC_PIO_BITS 7u
#define NTSC_PIO_CLOCKS 22u
#define NTSC_PIO_CLOCKS_PER_BIT (NTSC_PIO_CLOCKS / NTSC_PIO_BITS)
#define NTSC_PIO_EXTRA_CLOCKS (NTSC_PIO_CLOCKS % NTSC_PIO_BITS) // the only jitter
_Static_assert(CPU_CLOCK * 1000u * NTSC_PIO_BITS == 157500u * NTSC_PIO_CLOCKS,
               "PIO bit rate must be exactly 157.5 Mbit/s");

#define NTSC_FRAME_WIDTH       320
#define NTSC_FRAME_HEIGHT      240
#define NTSC_SAMPLES_PER_LINE  908 // 227 color cycles of 4 samples
#define NTSC_TOTAL_LINES       262
#define NTSC_VSYNC_LINES       10
#define NTSC_VBLANK_TOP        9   // leaves 3 blank lines before the next vsync
#define NTSC_HSYNC_WIDTH       68  // 4.7 us
#define NTSC_BACK_PORCH_SAMPLES 8
#define NTSC_COLOR_BURST_CYCLES 9
#define NTSC_ACTIVE_START      (NTSC_HSYNC_WIDTH + NTSC_BACK_PORCH_SAMPLES + \
                                NTSC_COLOR_BURST_CYCLES * 4 + 60)

// Signal levels 0..11 go out as that many high bits of a sample.
static inline uint16_t ntsc_level(const int32_t level) {
    return (uint16_t)((1u << (level <= 0 ? 0 : level >= 11 ? 11 : level)) - 1u);
}

// Level of luminance 0..255 plus a chroma part; black is level 2.
static inline int32_t ntsc_signal(const int32_t luminance, const int32_t chroma) {
    return (luminance * 1792 + chroma + 2 * 65536 + 32768) / 65536;
}

static inline int32_t ntsc_luminance_of(const uint32_t rgb) {
    return (150 * (int32_t)(rgb >> 8 & 0xffu) + 29 * (int32_t)(rgb & 0xffu) +
            77 * (int32_t)(rgb >> 16 & 0xffu) + 128) / 256;
}

#define NTSC_LEVEL_SYNC       ntsc_level(0)
#define NTSC_LEVEL_BLANK      ntsc_level(2)
#define NTSC_LEVEL_BURST_LOW  ntsc_level(1)
#define NTSC_LEVEL_BURST_HIGH ntsc_level(3)

// While DMA sends one line, the IRQ makes the other.
static uint16_t ntsc_scanline_buffers[2][NTSC_SAMPLES_PER_LINE] __attribute__((aligned(4)));
static size_t ntsc_scanline = 2;
static uint8_t ntsc_staging[NTSC_FRAME_WIDTH] __attribute__((aligned(4)));

// A pixel is two samples. Even pixels take subcarrier phases 0 and 90 degrees,
// odd pixels 180 and 270, so a pixel pair is one color cycle.
static uint32_t ntsc_palette_even[256];
static uint32_t ntsc_palette_odd[256];
// One sample a pixel: 80-column text and two-color 640-pixel modes.
static uint16_t ntsc_luminance[256];
// CGA composite modes: one sample a CGA dot, black or the foreground color.
static uint16_t ntsc_cga_luminance[16];

static uint ntsc_dma_data_channel;
static uintptr_t ntsc_dma_read_addresses[2] __attribute__((aligned(8)));

// The TV makes artifact color from the luminance pattern of these modes.
static inline bool ntsc_luminance_mode(const enum graphics_mode_t mode) {
    switch (mode) {
        case TEXTMODE_80x25_BW: case TEXTMODE_80x25_COLOR:
        case CGA_640x200x2: case HERC_640x480x2: case HERC_640x480x2_90:
        case VGA_640x480x2:
            return true;
        default:
            return false;
    }
}

static inline void ntsc_active_line(uint16_t *samples, const unsigned line) {
    const enum graphics_mode_t mode = graphics_source_mode();
    if (mode == COMPOSITE_160x200x16 || mode == COMPOSITE_160x200x16_force) {
        graphics_source_render_cga_bits16(line, NTSC_FRAME_HEIGHT, samples, ntsc_cga_luminance);
    } else if (ntsc_luminance_mode(mode)) {
        graphics_source_render_line16(line, GRAPHICS_TEXT_SAMPLES, NTSC_FRAME_HEIGHT,
                                      samples, ntsc_luminance);
    } else {
        graphics_source_render_line(line, NTSC_FRAME_WIDTH, NTSC_FRAME_HEIGHT, ntsc_staging);
        uint32_t *pairs = (uint32_t *)samples;
        for (unsigned x = 0; x < NTSC_FRAME_WIDTH; x += 2) {
            *pairs++ = ntsc_palette_even[ntsc_staging[x]];
            *pairs++ = ntsc_palette_odd[ntsc_staging[x + 1]];
        }
    }
}

// The two buffers keep their sync and burst between lines, so a line writes
// only what differs from the line two before it.
static inline void ntsc_generate_scanline(uint16_t *line, const size_t scanline) {
    uint16_t *sample = line;
    const size_t active = scanline - (NTSC_VSYNC_LINES + NTSC_VBLANK_TOP);
    if (active < NTSC_FRAME_HEIGHT) {
        ntsc_active_line(line + NTSC_ACTIVE_START, active);
    } else if (scanline < 2) { // vertical sync
        for (int j = 0; j < NTSC_SAMPLES_PER_LINE - NTSC_HSYNC_WIDTH; j++)
            *sample++ = NTSC_LEVEL_SYNC;
        while (sample < line + NTSC_SAMPLES_PER_LINE)
            *sample++ = NTSC_LEVEL_BLANK;
    } else if (scanline == NTSC_VSYNC_LINES || scanline == NTSC_VSYNC_LINES + 1) {
        // Horizontal sync and color burst for all lines up to the next vsync.
        for (int j = 0; j < NTSC_HSYNC_WIDTH; j++)
            *sample++ = NTSC_LEVEL_SYNC;
        for (int j = 0; j < NTSC_BACK_PORCH_SAMPLES; j++)
            *sample++ = NTSC_LEVEL_BLANK;
        for (int j = 0; j < NTSC_COLOR_BURST_CYCLES; j++) {
            *sample++ = NTSC_LEVEL_BLANK;
            *sample++ = NTSC_LEVEL_BURST_LOW;
            *sample++ = NTSC_LEVEL_BLANK;
            *sample++ = NTSC_LEVEL_BURST_HIGH;
        }
        while (sample < line + NTSC_SAMPLES_PER_LINE)
            *sample++ = NTSC_LEVEL_BLANK;
    } else if (active == NTSC_FRAME_HEIGHT || active == NTSC_FRAME_HEIGHT + 1) {
        // Blank the picture area for the lines after it.
        sample += NTSC_ACTIVE_START;
        for (int i = 0; i < NTSC_FRAME_WIDTH * 2; i++)
            *sample++ = NTSC_LEVEL_BLANK;
    }
}

void ntsc_composite_set_palette(const uint8_t index, const uint32_t color888) {
    const int32_t luminance = ntsc_luminance_of(color888);
    const int32_t blue = (int32_t)(color888 & 0xffu) - luminance;
    const int32_t red = (int32_t)(color888 >> 16 & 0xffu) - luminance;
    // (B-Y) and (R-Y) on phases 0 and 90 degrees; 180 and 270 are the negatives.
    const int32_t chroma0 = blue * 441 + red * 1361;
    const int32_t chroma90 = blue * 764 - red * 786;
    ntsc_palette_even[index] = ntsc_level(ntsc_signal(luminance, chroma0)) |
                               (uint32_t)ntsc_level(ntsc_signal(luminance, chroma90)) << 16;
    ntsc_palette_odd[index] = ntsc_level(ntsc_signal(luminance, -chroma0)) |
                              (uint32_t)ntsc_level(ntsc_signal(luminance, -chroma90)) << 16;
    ntsc_luminance[index] = ntsc_level(ntsc_signal(luminance, 0));
}

static void __time_critical_func(ntsc_dma_irq_handler)(void) {
    dma_hw->ints0 = 1u << ntsc_dma_data_channel;
    const size_t scanline = ntsc_scanline;
    ntsc_scanline = scanline + 1u == NTSC_TOTAL_LINES ? 0 : scanline + 1u;
    ntsc_generate_scanline(ntsc_scanline_buffers[scanline & 1u], scanline);
}

void ntsc_composite_init(void) {
    for (unsigned color = 0; color < 16; ++color) {
        ntsc_cga_luminance[color] =
                ntsc_level(ntsc_signal(ntsc_luminance_of(GRAPHICS_CGA_RGB(color)), 0));
    }

    // One loop, never stopped, so its phase never resets:
    //     set x, BITS - 1    [EXTRA_CLOCKS - 1]
    // bit:
    //     out pins, 1        [CLOCKS_PER_BIT - 2]   (+1 clock for the jmp)
    //     jmp x--, bit
    const uint16_t pio_words[3] = {
        (uint16_t)(pio_encode_set(pio_x, NTSC_PIO_BITS - 1u) |
                   pio_encode_delay(NTSC_PIO_EXTRA_CLOCKS - 1u)),
        (uint16_t)(pio_encode_out(pio_pins, 1u) |
                   pio_encode_delay(NTSC_PIO_CLOCKS_PER_BIT - 2u)),
        (uint16_t)pio_encode_jmp_x_dec(1u),
    };
    const struct pio_program pio_program = {
        .instructions = pio_words, .length = 3, .origin = -1, .pio_version = 0,
    };
    PIO pio;
    uint sm;
    uint offset;
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(
            &pio_program, &pio, &sm, &offset, NTSC_PIN, 1u, true)) {
        panic("NTSC: no free PIO state machine or 3 instruction words");
    }
    pio_sm_config sm_config = pio_get_default_sm_config();
    sm_config_set_wrap(&sm_config, offset, offset + 2u);
    sm_config_set_out_pins(&sm_config, NTSC_PIN, 1u);
    // Autopull every 11 bits. A 16-bit DMA write fills both FIFO halves; the low 11 bits go out.
    sm_config_set_out_shift(&sm_config, true, true, NTSC_SAMPLE_BITS);
    sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_TX);
    pio_sm_init(pio, sm, offset, &sm_config);
    pio_sm_set_pins_with_mask(pio, sm, 0u, 1u << NTSC_PIN);
    pio_sm_set_consecutive_pindirs(pio, sm, NTSC_PIN, 1u, true);
    pio_gpio_init(pio, NTSC_PIN);
    gpio_set_slew_rate(NTSC_PIN, GPIO_SLEW_RATE_FAST);
    pio_sm_set_enabled(pio, sm, true); // waits at sync level for the first DMA data

    // The data channel sends a line; the control channel then loads the other
    // buffer address from an 8-byte ring and restarts it.
    ntsc_dma_data_channel = dma_claim_unused_channel(true);
    const uint control_channel = dma_claim_unused_channel(true);
    ntsc_dma_read_addresses[0] = (uintptr_t)ntsc_scanline_buffers[1];
    ntsc_dma_read_addresses[1] = (uintptr_t)ntsc_scanline_buffers[0];

    dma_channel_config data = dma_channel_get_default_config(ntsc_dma_data_channel);
    channel_config_set_transfer_data_size(&data, DMA_SIZE_16);
    channel_config_set_read_increment(&data, true);
    channel_config_set_write_increment(&data, false);
    channel_config_set_dreq(&data, pio_get_dreq(pio, sm, true));
    channel_config_set_high_priority(&data, true);
    channel_config_set_chain_to(&data, control_channel);
    dma_channel_configure(ntsc_dma_data_channel, &data, &pio->txf[sm],
                          ntsc_scanline_buffers[0], NTSC_SAMPLES_PER_LINE, false);

    dma_channel_config control = dma_channel_get_default_config(control_channel);
    channel_config_set_transfer_data_size(&control, DMA_SIZE_32);
    channel_config_set_read_increment(&control, true);
    channel_config_set_write_increment(&control, false);
    channel_config_set_ring(&control, false, 3);
    channel_config_set_high_priority(&control, true);
    channel_config_set_chain_to(&control, ntsc_dma_data_channel);
    dma_channel_configure(control_channel, &control, &dma_hw->ch[ntsc_dma_data_channel].read_addr,
                          ntsc_dma_read_addresses, 1, false);

    ntsc_generate_scanline(ntsc_scanline_buffers[0], 0);
    ntsc_generate_scanline(ntsc_scanline_buffers[1], 1);

    dma_set_irq0_channel_mask_enabled(1u << ntsc_dma_data_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, ntsc_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(ntsc_dma_data_channel);
}
