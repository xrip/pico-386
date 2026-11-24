/* Graphics-mode INT 10h services for Pico-386.
 * Character and teletype semantics adapted from SeaBIOS
 * vgasrc/vgabios.c and vgafb.c. Framebuffer access is specific to Pico-386.
 * Copyright (C) 2009-2014 Kevin O'Connor <kevin@koconnor.net>
 * Copyright (C) 2001-2008 the LGPL VGABios developers Team
 * SPDX-License-Identifier: LGPL-3.0-only
 */
#include <stdbool.h>
#include "emulator/emulator.h"

typedef struct {
    unsigned cols, rows, height, page;
} text_grid;

static uint8_t tandy_pages;

static text_grid mode_grid(unsigned mode_number) {
    switch (mode_number) {
        case 0x08: return (text_grid){20, 25, 8};
        case 0x09: case 0x0d: case 0x13: return (text_grid){40, 25, 8};
        case 0x0a: case 0x0e: return (text_grid){80, 25, 8};
        case 0x10: return (text_grid){80, 25, 14};
        case 0x11: case 0x12: return (text_grid){80, 30, 16};
        default: return (text_grid){0};
    }
}

static text_grid graphics_grid(void) {
    text_grid g = mode_grid(videomode);
    unsigned h = RAM[0x485], rows = 1u + RAM[0x484];
    if (g.cols && h && h <= 32 && rows * h <= g.rows * g.height) {
        g.height = h;
        g.rows = rows;
    }
    return g;
}

static unsigned graphics_pages(void) {
    switch (videomode) {
        case 0x0d: return 8;
        case 0x0e: return 4;
        case 0x10: return 2;
        default: return 1;
    }
}

// Tandy interleaves banks. Unchained VGA can have shorter rows and a split screen.
static uint32_t *row_word(unsigned page, unsigned word, unsigned y) {
    unsigned addr;
    switch (videomode) {
        case 0x08: addr = tga_line_byte(tga_offset + (y % 2) * 8192, y / 2 * 80, word); break;
        case 0x09: addr = tga_line_byte(tga_offset + (y % 4) * 8192, y / 4 * 160, word); break;
        case 0x0a: addr = y * 320 + word; break;
        case 0x11: addr = y * 80 + word; break;
        case 0x13:
            if (vga_planar_mode) {
                unsigned words;
                addr = vga_x_line(y, &words) + word;
                if (word >= words) return NULL;
            } else {
                addr = vram_offset + y * 320 + word;
            }
            break;
        default: addr = page * (65536 / graphics_pages()) + y * 2 * crt_controller[0x13] + word; break;
    }
    return &VIDEORAM[addr & 0xffff];
}

static unsigned words_per_cell(void) {
    if (videomode <= 0x0a) return 4; // two pixels per byte
    if (videomode == 0x13) return vga_planar_mode ? 2 : 8;
    return 1; // eight pixels across the planes
}

static void draw_row(unsigned page, unsigned col, unsigned y, uint8_t bits, uint8_t color) {
    unsigned width = words_per_cell();
    bool invert = videomode != 0x13 && (color & 0x80);
    for (unsigned i = 0; i < width; ++i) {
        uint32_t *word = row_word(page, col * width + i, y);
        if (!word) continue;
        uint32_t value = 0, mask = 0xff;
        if (width == 1) {
            unsigned planes = videomode == 0x11 ? 1 : 4;
            for (unsigned plane = 0; plane < planes; ++plane)
                if (color & (1u << plane)) value |= (uint32_t)bits << (8 * plane);
            if (planes == 4) mask = UINT32_MAX;
        } else if (width == 4) {
            if (bits & 0x80) value |= (color & 15u) << 4;
            if (bits & 0x40) value |= color & 15u;
            bits <<= 2;
        } else {
            unsigned lanes = 8 / width;
            for (unsigned lane = 0; lane < lanes; ++lane) {
                if (bits & 0x80) value |= (uint32_t)color << (8 * lane);
                bits <<= 1;
            }
            if (lanes == 4) mask = UINT32_MAX;
        }
        if (invert) *word ^= value;
        else *word = (*word & ~mask) | value;
    }
}

// One BIOS pixel. Character rendering above writes a whole glyph row at a time.
static unsigned graphics_pixel(unsigned page, unsigned x, unsigned y, int ink) {
    unsigned width = words_per_cell(), per_word = 8 / width;
    uint32_t *dst = row_word(page, x / per_word, y);
    if (!dst) return 0;
    unsigned bit = x % per_word;
    if (width == 1) {
        unsigned result = 0;
        unsigned planes = videomode == 0x11 ? 1 : 4;
        for (unsigned i = 0; i < planes; ++i) {
            uint32_t mask = 1u << (8 * i + 7 - bit);
            if (*dst & mask) result |= 1u << i;
            if (ink >= 0) {
                if (ink & 0x80) { if (ink & (1u << i)) *dst ^= mask; }
                else if (ink & (1u << i)) *dst |= mask;
                else *dst &= ~mask;
            }
        }
        return result;
    }
    unsigned mask = width == 4 ? 15u : 255u;
    unsigned shift = width == 4 ? 4 * (1 - bit) : 8 * bit;
    unsigned prev = (*dst >> shift) & mask;
    if (ink >= 0) {
        unsigned value = (unsigned)ink & mask;
        if (mask == 15 && (ink & 0x80)) value ^= prev;
        *dst = (*dst & ~(mask << shift)) | (value << shift);
    }
    return prev;
}

typedef struct { unsigned x, y; } text_cursor;

static uint32_t glyph_start(text_grid g, unsigned ch) {
    uint32_t vec = readdw86(0x43 * 4);
    if (g.height == 8 && ch >= 128 && vec == VGA_FONT8) {
        vec = readdw86(0x1f * 4);
        ch -= 128;
    }
    return (vec >> 16) * 16 + (uint16_t)vec + ch * g.height;
}

static uint8_t read_character(text_grid g, text_cursor c) {
    if (c.x >= g.cols || c.y >= g.rows || g.height > 32) return 0;
    uint8_t glyph[32];
    for (unsigned y = 0; y < g.height; ++y) {
        unsigned bits = 0;
        for (unsigned x = 0; x < 8; ++x)
            bits = (bits << 1) | (graphics_pixel(g.page, 8*c.x+x, g.height*c.y+y, -1) != 0);
        glyph[y] = bits;
    }
    for (unsigned ch = 0; ch < 256; ++ch) {
        uint32_t font = glyph_start(g, ch);
        unsigned r = 0;
        while (r < g.height && glyph[r] == read86(font + r)) ++r;
        if (r == g.height) return ch;
    }
    return 0;
}

static void write_char(text_grid g, text_cursor *c, uint8_t ch, uint8_t color) {
    if (c->x >= g.cols || c->y >= g.rows) return;
    uint32_t font = glyph_start(g, ch);
    for (unsigned y = 0; y < g.height; ++y) {
        uint8_t bits = read86(font + y);
        draw_row(g.page, c->x, c->y * g.height + y, bits, color);
    }
    if (++c->x == g.cols) { c->x = 0; ++c->y; }
}

static void scroll_rect(text_grid g, unsigned left, unsigned top, unsigned right,
                        unsigned bottom, int lines, uint8_t fill) {
    if (right >= g.cols) right = g.cols - 1;
    if (bottom >= g.rows) bottom = g.rows - 1;
    if (left > right || top > bottom) return;
    unsigned words = words_per_cell();
    unsigned x0 = left * words, limit = (right + 1) * words;
    int first = top * g.height, end = (bottom + 1) * g.height;
    int step = lines < 0 ? -1 : 1;
    uint32_t background;
    if (videomode == 0x13) background = fill * 0x01010101u;
    else if (videomode <= 0x0a) background = (fill & 15) * 0x11;
    else {
        background = 0;
        for (unsigned plane = 0; plane < 4; ++plane)
            if (fill & (1u << plane)) background |= 0xffu << (plane * 8);
    }
    bool four_planes = (words_per_cell() == 1 && videomode != 0x11)
                    || (videomode == 0x13 && vga_planar_mode);
    uint32_t mask = four_planes ? UINT32_MAX : 0xff;
    for (int y = step > 0 ? first : end - 1; y >= first && y < end; y += step) {
        int source = y + lines * (int)g.height;
        for (unsigned x = x0; x < limit; ++x) {
            uint32_t *to = row_word(g.page, x, y);
            uint32_t *from = lines && source >= first && source < end ? row_word(g.page, x, source) : NULL;
            if (to) *to = (*to & ~mask) | ((from ? *from : background) & mask);
        }
    }
}

static void write_teletype(text_grid g, text_cursor *c, uint8_t ch, uint8_t color) {
    switch (ch) {
        case 7: bios_bell(); break;
        case 8: if (c->x) --c->x; break;
        case '\r': c->x = 0; break;
        case '\n': ++c->y; break;
        default: write_char(g, c, ch, color); break;
    }
    if (c->y >= g.rows) {
        c->y = g.rows - 1;
        scroll_rect(g, 0, 0, g.cols - 1, g.rows - 1, 1, 0);
    }
}

static bool set_mode(void) {
    unsigned requested = CPU_AL & 0x7f;
    text_grid g = mode_grid(requested);
    if (requested > 7 && !g.cols)
        return true; // An unsupported mode probe must leave the active screen intact.
    videomode = requested;
    // XT POST can clear the VGA part of the BDA before its first mode set.
    if (!RAM[0x489] && !RAM[0x488]) {
        RAM[0x488] = 9;
        RAM[0x489] = 0x51;
    }
    RAM[0x487] = (RAM[0x487] & 0x7f) | (CPU_AL & 0x80);
    writedw86(0x43 * 4, videomode == 0x10 ? VGA_FONT14 :
              (videomode <= 3 || videomode == 7 || videomode == 0x11 || videomode == 0x12 ? VGA_FONT16 : VGA_FONT8));
    vga_set_cpu_page_offset(0);
    vga_planar_mode = 0;
    tga_offset = 0x8000;
    crt_controller[0x13] = videomode == 0x0d ? 0x14 : 0x28;
    if (videomode <= 7) {
        cga_bios_set_mode(!(CPU_AL & 0x80));
        return true;
    }
    portout16(0x3c4, 0x0f02);
    portout16(0x3c4, videomode == 0x13 ? 0x0e04 : 0x0604);
    portout16(0x3ce, 0x0001);
    portout16(0x3ce, 0x0003);
    portout16(0x3ce, 0x0004);
    portout16(0x3ce, videomode == 0x13 ? 0x4005 : 0x0005);
    portout16(0x3ce, 0xff08);
    if (!(CPU_AL & 0x80)) memset(VIDEORAM, 0, sizeof(VIDEORAM));
    if (videomode == 0x13) {
        // Mode X programs change these standard mode 13h CRTC settings.
        static const uint8_t crtc[] = {
            0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0xbf, 0x1f, 0x00, 0x41, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x9c, 0x8e, 0x8f, 0x28, 0x40, 0x96, 0xb9, 0xa3, 0xff
        };
        memcpy(crt_controller, crtc, sizeof crtc);
    }
    RAM[0x449] = videomode;
    RAM[0x44a] = g.cols;
    RAM[0x44b] = 0;
    RAM[0x484] = g.rows - 1;
    writew86(0x44c, 65536 / graphics_pages());
    writew86(0x44e, 0);
    RAM[0x485] = g.height;
    RAM[0x486] = 0;
    RAM[0x462] = 0;
    memset(RAM + 0x450, 0, 16);
    vram_offset = 0;
    return true;
}

static uint8_t attribute(unsigned reg, bool write, uint8_t value) {
    uint8_t old = portin(0x3c0);
    portin(0x3da);
    portout(0x3c0, reg);
    if (write) portout(0x3c0, value);
    else value = portin(0x3c1);
    portin(0x3da);
    portout(0x3c0, old);
    portin(0x3da);
    return value;
}

static bool palette_service(void) {
    uint32_t addr = CPU_ES * 16u + CPU_DX;
    switch (CPU_AL) {
        case 0x00:
            if (videomode >= 8 && videomode <= 0x0a) tga_palette_map[CPU_BL & 15] = CPU_BH;
            else attribute(CPU_BL & 15, true, CPU_BH);
            return true;
        case 0x02:
            for (unsigned i = 0; i < 16; ++i) attribute(i, true, read86(addr++));
            attribute(0x11, true, read86(addr));
            return true;
        case 0x03:
            cga_blinking = CPU_BL ? 0x7f : 0xff;
            cga_blinking_lock = !CPU_BL;
            return true;
        case 0x01: attribute(0x11, true, CPU_BH); return true;
        case 0x07: CPU_BH = attribute(CPU_BL & 15, false, 0); return true;
        case 0x08: CPU_BH = attribute(0x11, false, 0); return true;
        case 0x09:
            for (unsigned i = 0; i < 16; ++i) write86(addr++, attribute(i, false, 0));
            write86(addr, attribute(0x11, false, 0));
            return true;
        case 0x13: {
            if (CPU_BL == 0) {
                uint8_t control = attribute(0x10, false, 0);
                attribute(0x10, true, (control & 0x7f) | ((CPU_BH & 1) << 7));
            } else if (CPU_BL == 1) {
                unsigned shift = attribute(0x10, false, 0) & 0x80 ? 0 : 2;
                attribute(0x14, true, (CPU_BH << shift) & 15);
            }
            return true;
        }
        case 0x1a:
            CPU_BL = attribute(0x10, false, 0) >> 7;
            CPU_BH = (attribute(0x14, false, 0) & 15) >> (CPU_BL ? 0 : 2);
            return true;
        case 0x18: portout(0x3c6, CPU_BL); return true;
        case 0x19: CPU_BL = portin(0x3c6); return true;
        case 0x1b:
            for (unsigned i = CPU_BX; i < 256 && i < (unsigned)CPU_BX + CPU_CX; ++i) {
                uint32_t c = vga_palette[i];
                unsigned v = (((c >> 16) & 255)*77 + ((c >> 8) & 255)*151 + (c & 255)*28 + 128) >> 8;
                vga_set_dac_color(i, (v & 0xfc) * 0x010101u);
            }
            return true;
        case 0x10:
            vga_set_dac_color(CPU_BL, rgb((CPU_DH & 63) << 2, (CPU_CH & 63) << 2, (CPU_CL & 63) << 2));
            return true;
        case 0x15: {
            uint32_t color = vga_palette[CPU_BL];
            CPU_DH = (color >> 18) & 63;
            CPU_CH = (color >> 10) & 63;
            CPU_CL = (color >> 2) & 63;
            return true;
        }
        case 0x12: case 0x17:
            for (unsigned i = CPU_BX; i < 256 && i < (unsigned)CPU_BX + CPU_CX; ++i) {
                if (CPU_AL == 0x12) {
                    unsigned r = (read86(addr++) & 63) << 2;
                    unsigned g = (read86(addr++) & 63) << 2;
                    unsigned b = (read86(addr++) & 63) << 2;
                    vga_set_dac_color(i, rgb(r, g, b));
                } else {
                    uint32_t color = vga_palette[i];
                    write86(addr++, (color >> 18) & 63);
                    write86(addr++, (color >> 10) & 63);
                    write86(addr++, (color >> 2) & 63);
                }
            }
            return true;
        default: return false;
    }
}

static bool font_service(void) {
    uint32_t vec;
    switch (CPU_AL) {
        case 0x11: case 0x12: case 0x14:
            if (videomode > 3 && videomode != 7) return false;
            cga_bios_text_font(CPU_AL == 0x11 ? 14 : CPU_AL == 0x12 ? 8 : 16);
            return true;
        case 0x20:
            writedw86(0x1f * 4, ((uint32_t)CPU_ES << 16) | CPU_BP);
            return true;
        case 0x21: case 0x22: case 0x23: case 0x24: {
            text_grid g = mode_grid(videomode);
            unsigned height = CPU_AL == 0x21 ? CPU_CX : CPU_AL == 0x22 ? 14 : CPU_AL == 0x23 ? 8 : 16;
            unsigned rows = CPU_BL == 0 ? CPU_DL : CPU_BL == 1 ? 14 : CPU_BL == 3 ? 43 : 25;
            if (!g.cols || !rows || !height || height > 32 || rows * height > g.rows * g.height) return false;
            vec = CPU_AL == 0x21 ? ((uint32_t)CPU_ES << 16) | CPU_BP :
                  height == 14 ? VGA_FONT14 : height == 8 ? VGA_FONT8 : VGA_FONT16;
            writedw86(0x43 * 4, vec);
            RAM[0x484] = rows - 1;
            RAM[0x485] = height;
            return true;
        }
        case 0x30:
            switch (CPU_BH) {
                case 0: vec = readdw86(0x1f * 4); break;
                case 1: vec = readdw86(0x43 * 4); break;
                case 2: case 5: vec = VGA_FONT14; break;
                case 3: vec = VGA_FONT8; break;
                case 4: vec = VGA_FONT8 + 1024; break;
                case 6: case 7: vec = VGA_FONT16; break;
                default: return false;
            }
            load_segment(reges, vec >> 16);
            CPU_BP = (uint16_t)vec;
            CPU_CX = RAM[0x485];
            CPU_DL = RAM[0x484];
            return true;
        default: return false;
    }
}

static bool adapter_info(void) {
    if (CPU_BX) return false;
    uint32_t addr = ES_BASE + CPU_DI;
    for (unsigned i = 0; i < 64; ++i) write86(addr + i, 0);
    writedw86(addr, VGA_STATIC_INFO);
    for (unsigned i = 0; i < 30; ++i) write86(addr + 4 + i, RAM[0x449 + i]);
    for (unsigned i = 0; i < 3; ++i) write86(addr + 0x22 + i, RAM[0x484 + i]);
    write86(addr + 0x25, RAM[0x48a]);
    unsigned colors = videomode == 0x13 ? 256 : videomode == 4 || videomode == 5 ? 4 :
                      videomode == 6 || videomode == 0x11 ? 2 : 16;
    writew86(addr + 0x27, colors);
    unsigned pages = graphics_pages();
    if (videomode < 4) {
        unsigned size = readw86(0x44c);
        pages = size ? 32768 / size : 1;
        if (pages > 8) pages = 8;
    }
    write86(addr + 0x29, pages);
    write86(addr + 0x2a, videomode == 0x10 ? 1 : videomode == 0x11 || videomode == 0x12 ? 3 :
                           videomode <= 3 || videomode == 7 ? 2 : 0);
    write86(addr + 0x31, 3); // 256 KiB of VGA memory
    CPU_AL = 0x1b;
    return true;
}

// INT 10h/1Ch buffers are private to the BIOS that saves them. Save only
// the groups the caller requests, preserving the indexed-port selectors.
static uint8_t state_byte(uint32_t *at, bool restore, uint8_t value) {
    if (restore) value = read86(*at);
    else write86(*at, value);
    ++*at;
    return value;
}

static void indexed_state(uint32_t *at, bool restore, unsigned port, unsigned count) {
    uint8_t index = state_byte(at, restore, portin(port));
    for (unsigned i = 0; i < count; ++i) {
        portout(port, i);
        uint8_t v = state_byte(at, restore, portin(port + 1));
        if (restore) portout(port + 1, v);
    }
    portout(port, index);
}

static bool video_state(void) {
    unsigned groups = CPU_CX, op = CPU_AL;
    if ((groups & ~7u) || op > 2) return false;
    enum { HW_BYTES = 3 + (1+32) + (1+5) + (1+9) + 21 + 1,
           BDA_BYTES = 30 + 8 + 8, DAC_BYTES = 1 + 768 };
    if (!op) {
        unsigned bytes = (groups & 1 ? HW_BYTES : 0) + (groups & 2 ? BDA_BYTES : 0)
                       + (groups & 4 ? DAC_BYTES : 0);
        CPU_BX = (bytes + 63) / 64;
    } else {
        uint32_t at = ES_BASE + CPU_BX;
        bool restore = op == 2;
        if (groups & 1) {
            uint8_t m = state_byte(&at, restore, videomode);
            uint8_t misc = state_byte(&at, restore, portin(0x3cc));
            uint8_t enable = state_byte(&at, restore, portin(0x3c3));
            if (restore) { videomode = m; portout(0x3c2, misc); portout(0x3c3, enable); }
            indexed_state(&at, restore, 0x3d4, 32);
            indexed_state(&at, restore, 0x3c4, 5);
            indexed_state(&at, restore, 0x3ce, 9);
            for (unsigned i = 0; i < 21; ++i) {
                uint8_t old = attribute(i, false, 0);
                uint8_t value = state_byte(&at, restore, old);
                if (restore && old != value) {
                    uint32_t saved_color = vga_palette[i];
                    attribute(i, true, value);
                    if (i < 16) vga_set_dac_color(i, saved_color);
                }
            }
            uint8_t planar = state_byte(&at, restore, vga_planar_mode);
            if (restore) { videomode = m; vga_planar_mode = planar; }
        }
        if (groups & 2) {
            for (unsigned i = 0x449; i < 0x467; ++i) RAM[i] = state_byte(&at, restore, RAM[i]);
            for (unsigned i = 0x484; i < 0x48c; ++i) RAM[i] = state_byte(&at, restore, RAM[i]);
            for (unsigned j = 0; j < 2; ++j) {
                unsigned v = (j ? 0x43 : 0x1f) * 4;
                for (unsigned i = v; i < v+4; ++i) RAM[i] = state_byte(&at, restore, RAM[i]);
            }
            if (restore) videomode = RAM[0x449] & 0x7f;
        }
        if (groups & 4) {
            uint8_t mask = state_byte(&at, restore, portin(0x3c6));
            if (restore) portout(0x3c6, mask);
            for (unsigned i = 0; i < 256; ++i) {
                uint32_t c = vga_palette[i];
                unsigned r = state_byte(&at, restore, c >> 16);
                unsigned g = state_byte(&at, restore, c >> 8);
                unsigned b = state_byte(&at, restore, c);
                if (restore) vga_set_dac_color(i, rgb(r,g,b));
            }
        }
    }
    CPU_AL = 0x1c;
    return true;
}

bool vga_bios_int10(void) {
    if ((unsigned)videomode <= 7 && CPU_AH && (CPU_AH <= 0x0f || CPU_AH == 0x13) && cga_bios_int10()) return true;
    switch (CPU_AH) {
        case 0x01:
            writew86(0x460, CPU_CX);
            portout16(0x3d4, 10 | ((unsigned)CPU_CH << 8));
            portout16(0x3d4, 11 | ((unsigned)CPU_CL << 8));
            return true;
        case 0x04: CPU_AH = 0; return true;
        case 0x08: case 0x0c: case 0x0d: {
            text_grid g = graphics_grid();
            if (!g.cols) return false;
            g.page = graphics_pages() == 1 ? 0 : CPU_BH;
            if (g.page >= graphics_pages()) return true;
            if (CPU_AH == 8) {
                text_cursor c = { RAM[0x450 + 2*g.page], RAM[0x451 + 2*g.page] };
                CPU_AX = read_character(g, c);
            } else if (CPU_CX < g.cols * 8 && CPU_DX < g.rows * g.height) {
                unsigned pixel = graphics_pixel(g.page, CPU_CX, CPU_DX, CPU_AH == 0x0c ? CPU_AL : -1);
                if (CPU_AH == 0x0d) CPU_AL = pixel;
            }
            return true;
        }
        case 0x06: case 0x07: {
            text_grid g = graphics_grid();
            if (!g.cols) return false;
            g.page = RAM[0x462] % graphics_pages();
            scroll_rect(g, CPU_CL, CPU_CH, CPU_DL, CPU_DH, CPU_AH == 6 ? CPU_AL : -(int)CPU_AL, CPU_BH);
            return true;
        }
        case 0x02: case 0x03:
            if (CPU_BH >= 8) return true;
            if (CPU_AH == 2) {
                RAM[0x450 + 2 * CPU_BH] = CPU_DL;
                RAM[0x451 + 2 * CPU_BH] = CPU_DH;
            } else {
                CPU_DX = readw86(0x450 + 2 * CPU_BH);
                CPU_CX = readw86(0x460);
            }
            return true;
        case 0x13: {
            text_grid g = graphics_grid();
            if (!g.cols) return false;
            g.page = graphics_pages() == 1 ? 0 : CPU_BH;
            if (g.page >= graphics_pages()) return true;
            text_cursor c = {CPU_DL, CPU_DH};
            uint16_t off = CPU_BP;
            uint8_t color = CPU_BL;
            for (unsigned i = 0; i < CPU_CX; ++i) {
                uint8_t ch = read86(ES_BASE + off++);
                if (CPU_AL & 2) color = read86(ES_BASE + off++);
                write_teletype(g, &c, ch, color);
            }
            if (CPU_AL & 1) { RAM[0x450 + 2*g.page] = c.x; RAM[0x451 + 2*g.page] = c.y; }
            return true;
        }
        case 0x09: case 0x0a: case 0x0e: {
            text_grid g = graphics_grid();
            if (!g.cols) return false;
            g.page = CPU_AH == 0x0e ? RAM[0x462] : CPU_BH;
            if (graphics_pages() == 1) g.page = 0;
            if (g.page >= graphics_pages()) return true;
            text_cursor c = { RAM[0x450 + 2*g.page], RAM[0x451 + 2*g.page] };
            if (CPU_AH == 0x0e) {
                write_teletype(g, &c, CPU_AL, CPU_BL);
                CURSOR_X = c.x;
                CURSOR_Y = c.y;
            } else {
                for (unsigned i = 0; i < CPU_CX && c.y < g.rows; ++i)
                    write_char(g, &c, CPU_AL, CPU_BL);
            }
            return true;
        }
        case 0x0f:
            if (videomode < 8) return false;
            CPU_AL = videomode | (RAM[0x487] & 0x80);
            CPU_AH = RAM[0x44a];
            CPU_BH = RAM[0x462];
            return true;
        case 0x00: return set_mode();
        case 0x05: {
            if (CPU_AL < 0x80 && graphics_grid().cols) {
                unsigned pages = graphics_pages();
                if (CPU_AL < pages) {
                    unsigned start = CPU_AL * (65536 / pages);
                    RAM[0x462] = CPU_AL;
                    writew86(0x44e, start);
                    portout16(0x3d4, 12 | ((start >> 1) & 0xff00));
                    portout16(0x3d4, 13 | ((start >> 1) << 8));
                }
                return true;
            }
            uint8_t page = tandy_pages;
            switch (CPU_AL) {
                case 0x80: CPU_BH = page & 7; CPU_BL = (page >> 3) & 7; return true;
                case 0x81: page = (page & 0xc7) | ((CPU_BL & 7) << 3); break;
                case 0x82: page = (page & 0xf8) | (CPU_BH & 7); break;
                case 0x83: page = (page & 0xc0) | (CPU_BH & 7) | ((CPU_BL & 7) << 3); break;
                default: return false; // standard display pages belong to the ROM
            }
            tga_portout(0x3df, page);
            tandy_pages = page;
            return true;
        }
        case 0x10: return palette_service();
        case 0x11: return font_service();
        case 0x12:
            if (CPU_BL == 0x30 && CPU_AL <= 2) {
                static const uint8_t bits[] = {0x80, 0, 0x10};
                RAM[0x489] = (RAM[0x489] & ~0x90) | bits[CPU_AL];
                RAM[0x488] = (RAM[0x488] & 0xf0) | (CPU_AL == 0 ? 8 : 9);
                CPU_AL = 0x12;
                return true;
            }
            if (CPU_BL == 0x32 && CPU_AL <= 1) {
                portout(0x3c3, !CPU_AL);
                CPU_AL = 0x12;
                return true;
            }
            if (CPU_BL != 0x10) return false;
            CPU_BX = (RAM[0x463] == 0xb4 ? 0x0100 : 0) | 3;
            CPU_CX = RAM[0x488] & 15;
            return true;
        case 0x1a:
            if (CPU_AL == 0) CPU_BX = RAM[0x48a];
            else if (CPU_AL == 1) RAM[0x48a] = CPU_BL;
            else return false;
            CPU_AL = 0x1a;
            return true;
        case 0x1b: return adapter_info();
        case 0x1c: return video_state();
        default: return false;
    }
}
