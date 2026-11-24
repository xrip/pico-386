/**
 * MIT License
 *
 * Copyright (c) 2022 Vincent Mistler
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "audio.h"
#include <stdlib.h>
#include "audio_i2s.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

enum { DATA_PIN = 9, BCLK_PIN = 10, LRCLK_PIN = 11, BLOCK = 64 };
static uint dma_channel;
static uint32_t pwm_period;
static uint32_t (*blocks)[BLOCK];
static uint32_t silence;
static volatile uint32_t consumed, produced;
static uint position;
static bool playing;

// M2P2 load test, as used by pico-spec. Discharge the PWM filter on warm boot.
static bool detect_i2s(void) {
    const uint32_t pins = (1u << DATA_PIN) | (1u << BCLK_PIN);
    gpio_init_mask(pins);
    gpio_clr_mask(pins);
    gpio_set_dir_out_masked(pins);
    sleep_ms(50);
    gpio_pull_down(DATA_PIN);
    gpio_pull_down(BCLK_PIN);
    gpio_set_dir_in_masked(pins);
    sleep_ms(33);
    bool high_under_pulldown = gpio_get_all() & pins;
    gpio_pull_up(DATA_PIN);
    gpio_pull_up(BCLK_PIN);
    sleep_ms(33);
    bool follows_pullup = gpio_get_all() & pins;
    bool coupled = false;
    if (!high_under_pulldown && !follows_pullup) {
        gpio_pull_down(BCLK_PIN);
        gpio_put(DATA_PIN, true);
        gpio_set_dir(DATA_PIN, GPIO_OUT);
        sleep_ms(33);
        coupled = gpio_get(BCLK_PIN);
    }
    gpio_deinit(DATA_PIN);
    gpio_deinit(BCLK_PIN);
    return !high_under_pulldown && (follows_pullup || coupled);
}

static void __time_critical_func(audio_dma_done)(void) {
    dma_irqn_acknowledge_channel(2, dma_channel);
    if (playing) ++consumed;
    playing = produced != consumed;
    dma_channel_config dma = dma_get_channel_config(dma_channel);
    channel_config_set_read_increment(&dma, playing);
    dma_channel_set_config(dma_channel, &dma, false);
    const uint32_t *next = playing ? blocks[consumed % 2] : &silence;
    dma_channel_transfer_from_buffer_now(dma_channel, next, BLOCK);
}

void vga_audio_init(uint32_t rate) {
    blocks = malloc(2 * sizeof(*blocks));
    if (!blocks) panic("VGA audio allocation failed");
    dma_channel = dma_claim_unused_channel(true);
    dma_channel_config dma = dma_channel_get_default_config(dma_channel);
    volatile uint32_t *destination;
    if (detect_i2s()) {
        PIO pio = pio1;
        uint sm = pio_claim_unused_sm(pio, true);
        uint offset = pio_add_program(pio, &audio_i2s_program);
        for (uint pin = DATA_PIN; pin <= LRCLK_PIN; ++pin) pio_gpio_init(pio, pin);
        audio_i2s_program_init(pio, sm, offset, DATA_PIN, BCLK_PIN);
        pio_sm_set_clkdiv(pio, sm, (float)clock_get_hz(clk_sys) / (64u * rate));
        channel_config_set_dreq(&dma, pio_get_dreq(pio, sm, true));
        destination = &pio->txf[sm];
        pio_sm_set_enabled(pio, sm, true);
    } else {
        uint slice = pwm_gpio_to_slice_num(BCLK_PIN);
        pwm_period = (clock_get_hz(clk_sys) + rate / 2) / rate;
        pwm_config cfg = pwm_get_default_config();
        pwm_config_set_wrap(&cfg, pwm_period - 1);
        pwm_init(slice, &cfg, false);
        uint32_t mid = pwm_period / 2;
        pwm_set_both_levels(slice, mid, mid);
        gpio_set_function(BCLK_PIN, GPIO_FUNC_PWM);
        gpio_set_function(LRCLK_PIN, GPIO_FUNC_PWM);
        silence = (mid << 16) | mid;
        channel_config_set_dreq(&dma, pwm_get_dreq(slice));
        destination = &pwm_hw->slice[slice].cc;
        pwm_set_enabled(slice, true);
    }
    channel_config_set_transfer_data_size(&dma, DMA_SIZE_32);
    channel_config_set_read_increment(&dma, false);
    dma_channel_configure(dma_channel, &dma, destination, &silence, BLOCK, false);
    irq_set_exclusive_handler(DMA_IRQ_2, audio_dma_done);
    dma_irqn_set_channel_enabled(2, dma_channel, true);
    irq_set_enabled(DMA_IRQ_2, true);
    dma_channel_start(dma_channel);
}

bool vga_audio_has_room(void) {
    return produced - consumed < 2;
}

void vga_audio_write(int16_t left, int16_t right) {
    uint32_t l = (uint16_t)left, r = (uint16_t)right;
    if (pwm_period) {
        l = ((l ^ 0x8000u) * pwm_period) >> 16;
        r = ((r ^ 0x8000u) * pwm_period) >> 16;
    }
    // I2S shifts left first. PWM B (GPIO11) is left, A (GPIO10) is right.
    blocks[produced % 2][position++] = (l << 16) | r;
    if (position == BLOCK) {
        position = 0;
        __dmb();
        ++produced;
    }
}
