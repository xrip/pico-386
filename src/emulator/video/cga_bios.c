/* Native PC/XT video services (INT 10h, AH=00h..0Fh).
 * CGA has one packed byte per VRAM word in this emulator.
 */
#include "emulator/emulator.h"

static uint16_t bda_word(unsigned addr) {
    return RAM[addr] | ((uint16_t)RAM[addr + 1] << 8);
}

static void bda_put_word(unsigned addr, unsigned value) {
    RAM[addr] = value;
    RAM[addr + 1] = value >> 8;
}

static bool text_mode(void) { return videomode < 4 || videomode == 7; }
static unsigned columns(void) { return (videomode < 2 || videomode == 4 || videomode == 5) ? 40 : 80; }
static unsigned rows(void) {
    unsigned count = RAM[0x484] + 1u;
    return text_mode() && count <= 50 ? count : 25;
}
static unsigned page_size(void) { return text_mode() ? ((2 * columns() * rows() + 2047) & ~2047u) : 16384; }
static unsigned video_base(void) { return videomode == 7 ? 0 : 0x8000; }
static bool valid_page(unsigned page) { return page == 0 || (videomode < 4 && page < 8 && page < 32768 / page_size()); }

static void crtc_write(unsigned reg, uint8_t data) {
    portout16(bda_word(0x463), reg | ((uint16_t)data << 8));
}

static void crtc_address(unsigned reg, unsigned pos) {
    crtc_write(reg, pos >> 8);
    crtc_write(reg + 1, pos);
}

static uint16_t cursor(unsigned page) { return bda_word(0x450 + page * 2); }

static void set_cursor(unsigned page, uint16_t pos) {
    bda_put_word(0x450 + 2 * page, pos);
    if (page == RAM[0x462])
        crtc_address(14, bda_word(0x44e) / 2 + (pos >> 8) * columns() + (pos & 255));
}

static uint32_t vram_byte(unsigned value) { return (uint8_t)value * 0x01010101u; }

// Select one of the three resident VGA text fonts. A 400-line display
// gives 25 rows with the 16-row font and 50 rows with the 8-row font.
void cga_bios_text_font(unsigned height) {
    unsigned lines = RAM[0x489] & 0x80 ? 200 : RAM[0x489] & 0x10 ? 400 : 350;
    RAM[0x484] = lines / height - 1;
    bda_put_word(0x485, height);
    bda_put_word(0x44c, page_size());
    writedw86(0x43 * 4, height == 8 ? VGA_FONT8 : height == 14 ? VGA_FONT14 : VGA_FONT16);
    unsigned page = RAM[0x462];
    if (!valid_page(page)) page = 0;
    RAM[0x462] = page;
    bda_put_word(0x44e, page * page_size());
    crtc_address(12, page * page_size() / 2);
    crtc_write(9, (crt_controller[9] & 0xe0) | (height - 1));
    set_cursor(page, cursor(page));
}

void cga_bios_set_mode(bool clear) {
    static const uint8_t mode_control[] = {0x2c,0x28,0x2d,0x29,0x2a,0x2e,0x1e,0x29};
    const unsigned base = videomode == 7 ? 0x3b4 : 0x3d4;
    // The caller may replace the 6845 parameter table through INT 1Dh.
    uint32_t vec = readdw86(0x1d * 4);
    unsigned table = vec ? ((vec >> 16) * 16 + (uint16_t)vec) : 0xff0a4;
    table += (videomode < 2 ? 0 : videomode < 4 ? 1 : videomode < 7 ? 2 : 3) * 16;
    RAM[0x449] = videomode;
    bda_put_word(0x44a, columns());
    memset(RAM + 0x44e, 0, 18); // display start and eight cursor positions
    RAM[0x462] = 0;
    bda_put_word(0x463, base);
    RAM[0x465] = mode_control[videomode];
    RAM[0x466] = videomode == 6 ? 0x3f : 0x30;
    RAM[0x484] = 24;
    bda_put_word(0x485, 8);
    bda_put_word(0x44c, page_size());
    // GLaBIOS also keeps the text-memory segment for its internal POST routines.
    bda_put_word(0x4ae, videomode == 7 ? 0xb000 : 0xb800);
    for (unsigned i = 0; i < 16; ++i) crtc_write(i, read86(table + i));
    bda_put_word(0x460, ((unsigned)crt_controller[10] << 8) | crt_controller[11]);
    portout(base + 4, RAM[0x465]);
    if (videomode != 7) portout(base + 5, RAM[0x466]);
    if (text_mode()) {
        unsigned height = RAM[0x489] & 0x80 ? 8 : RAM[0x489] & 0x10 ? 16 : 14;
        cga_bios_text_font(height);
    }
    if (!clear) return;
    uint32_t *dst = VIDEORAM + video_base();
    if (text_mode()) {
        unsigned size = videomode == 7 ? 4096 : 16384;
        for (unsigned i = 0; i < size; i += 2) {
            dst[i] = vram_byte(' ');
            dst[i + 1] = vram_byte(7);
        }
    } else memset(dst, 0, 16384 * sizeof(*dst));
}

static uint32_t *text_cell(unsigned page, unsigned x, unsigned y) {
    return VIDEORAM + video_base() + page_size() * page + 2 * (y * columns() + x);
}

static uint32_t *graphics_byte(unsigned xbyte, unsigned y) {
    return VIDEORAM + 0x8000 + (y & 1) * 8192 + (y / 2) * 80 + xbyte;
}

static uint32_t glyph_address(unsigned ch) {
    if (ch < 128) return 0xffa6e + 8 * ch;
    uint32_t vec = readdw86(0x7c);
    return vec ? (vec >> 16) * 16 + (uint16_t)vec + 8 * (ch - 128) : 0;
}

static void write_chars(unsigned page, unsigned x, unsigned y, uint8_t ch,
                        uint8_t color, unsigned count, bool attr) {
    unsigned cols = columns();
    if (x >= cols || y >= rows()) return;
    unsigned remaining = (rows() - y) * cols - x;
    if (count > remaining) count = remaining;
    if (text_mode()) {
        uint32_t *dst = text_cell(page, x, y);
        uint32_t chr = vram_byte(ch), ink = vram_byte(color);
        for (; count; --count, dst += 2) {
            dst[0] = chr;
            if (attr) dst[1] = ink;
        }
        return;
    }
    uint32_t font = glyph_address(ch);
    unsigned bpc = videomode == 6 ? 1 : 2;
    uint32_t glyph[8][2];
    for (unsigned r = 0; r < 8; ++r) {
        uint8_t bits = font ? read86(font + r) : 0;
        if (bpc == 1) glyph[r][0] = vram_byte((color & 1) ? bits : 0);
        else {
            unsigned packed = 0;
            for (unsigned i = 0; i < 8; ++i)
                packed = (packed << 2) | ((bits & (0x80 >> i)) ? color & 3 : 0);
            glyph[r][0] = vram_byte(packed >> 8);
            glyph[r][1] = vram_byte(packed);
        }
    }
    while (count--) {
        for (unsigned r = 0; r < 8; ++r) {
            uint32_t *dst = graphics_byte(x * bpc, y * 8 + r);
            for (unsigned i = 0; i < bpc; ++i) {
                if (color & 0x80) dst[i] ^= glyph[r][i];
                else dst[i] = glyph[r][i];
            }
        }
        if (++x == cols) { x = 0; ++y; }
    }
}

static uint8_t read_glyph(unsigned x, unsigned y) {
    uint8_t rows[8];
    for (unsigned r = 0; r < 8; ++r) {
        if (videomode == 6) rows[r] = *graphics_byte(x, y * 8 + r);
        else {
            uint32_t *src = graphics_byte(x * 2, y * 8 + r);
            unsigned packed = ((uint8_t)src[0] << 8) | (uint8_t)src[1];
            unsigned bits = 0;
            for (unsigned i = 0; i < 8; ++i) {
                bits = (bits << 1) | ((packed & 0xc000) != 0);
                packed <<= 2;
            }
            rows[r] = bits;
        }
    }
    for (unsigned ch = 0; ch < 256; ++ch) {
        uint32_t font = glyph_address(ch);
        if (!font) break;
        unsigned r = 0;
        while (r < 8 && read86(font + r) == rows[r]) ++r;
        if (r == 8) return ch;
    }
    return 0;
}

static void scroll(unsigned page, unsigned left, unsigned top, unsigned right, unsigned bottom,
                   int lines, uint8_t fill) {
    unsigned cols = columns();
    if (right >= cols) right = cols - 1;
    if (bottom >= rows()) bottom = rows() - 1;
    if (left > right || top > bottom) return;
    bool text = text_mode();
    unsigned stride = !text && videomode == 6 ? 1 : 2;
    unsigned width = (right - left + 1) * stride;
    int first = top, end = bottom + 1;
    if (!text) { first *= 8; end *= 8; lines *= 8; }
    int step = lines < 0 ? -1 : 1;
    uint32_t background = vram_byte(fill);
    for (int y = step > 0 ? first : end - 1; y >= first && y < end; y += step) {
        int source = y + lines;
        uint32_t *dst = text ? text_cell(page, left, y) : graphics_byte(left * stride, y);
        if (lines && source >= first && source < end) {
            const uint32_t *src = text ? text_cell(page, left, source) : graphics_byte(left * stride, source);
            memcpy(dst, src, width * sizeof(*dst));
        } else if (text) {
            for (unsigned i = 0; i < width; i += 2) {
                dst[i] = vram_byte(' ');
                dst[i + 1] = background;
            }
        } else {
            for (unsigned i = 0; i < width; ++i) dst[i] = background;
        }
    }
}

static unsigned emit(unsigned page, unsigned pos, uint8_t ch, uint8_t color, bool attr) {
    unsigned x = pos & 255, y = pos >> 8;
    switch (ch) {
        case 7: bios_bell(); break;
        case 8: if (x) --x; break;
        case 13: x = 0; break;
        case 10: ++y; break;
        default:
            write_chars(page, x, y, ch, color, 1, attr);
            if (++x >= columns()) { x = 0; ++y; }
    }
    if (y >= rows()) {
        y = rows() - 1;
        uint8_t fill = text_mode() ? text_cell(page, x < columns() ? x : 0, y)[1] : 0;
        scroll(page, 0, 0, columns() - 1, y, 1, fill);
    }
    return (y << 8) | x;
}

bool cga_bios_int10(void) {
    unsigned page = text_mode() ? CPU_BH : 0;
    switch (CPU_AH) {
        case 1:
            bda_put_word(0x460, CPU_CX);
            crtc_address(10, CPU_CX);
            return true;
        case 2:
            if (valid_page(page)) set_cursor(page, CPU_DX);
            return true;
        case 3:
            CPU_CX = bda_word(0x460);
            CPU_DX = valid_page(page) ? cursor(page) : 0;
            return true;
        case 4: CPU_AH = 0; return true; // no light pen attached
        case 5:
            if (CPU_AL >= 0x80) return false; // Tandy page registers
            if (valid_page(CPU_AL)) {
                unsigned start = page_size() * CPU_AL;
                RAM[0x462] = CPU_AL;
                bda_put_word(0x44e, start);
                crtc_address(12, start / 2);
                set_cursor(CPU_AL, cursor(CPU_AL));
            }
            return true;
        case 6: case 7:
            scroll(text_mode() ? RAM[0x462] & 7 : 0, CPU_CL, CPU_CH, CPU_DL, CPU_DH, CPU_AH == 6 ? CPU_AL : -(int)CPU_AL, CPU_BH);
            return true;
        case 8: case 9: case 10: {
            if (!valid_page(page)) return true;
            unsigned pos = cursor(page), x = pos & 255, y = pos >> 8;
            if (x >= columns() || y >= rows()) return true;
            if (CPU_AH == 8) {
                if (text_mode()) {
                    const uint32_t *cell = text_cell(page, x, y);
                    CPU_AX = (uint8_t)cell[0] | ((uint8_t)cell[1] << 8);
                } else CPU_AL = read_glyph(x, y);
            } else write_chars(page, x, y, CPU_AL, CPU_BL, CPU_CX, CPU_AH == 9);
            return true;
        }
        case 11:
            if (!CPU_BH) RAM[0x466] = (RAM[0x466] & 0xe0) | (CPU_BL & 31);
            else RAM[0x466] = (RAM[0x466] & 0xdf) | ((CPU_BL & 1) << 5);
            portout(0x3d9, RAM[0x466]);
            return true;
        case 12: case 13: {
            if (text_mode() || CPU_DX >= 200 || CPU_CX >= columns() * 8) return true;
            unsigned bits = videomode == 6 ? 1 : 2;
            unsigned bit = CPU_CX * bits;
            unsigned shift = 8 - bits - bit % 8;
            unsigned mask = (1u << bits) - 1;
            uint32_t *dst = graphics_byte(bit / 8, CPU_DX);
            unsigned old = (uint8_t)*dst;
            unsigned ink = (CPU_AL & mask) << shift;
            if (CPU_AH == 13) CPU_AL = (old >> shift) & mask;
            else *dst = vram_byte((CPU_AL & 0x80) ? old ^ ink : (old & ~(mask << shift)) | ink);
            return true;
        }
        case 14:
            page = text_mode() ? RAM[0x462] & 7 : 0;
            set_cursor(page, emit(page, cursor(page), CPU_AL, CPU_BL, false));
            return true;
        case 0x13: {
            if (!valid_page(page)) return true;
            unsigned pos = CPU_DX;
            uint16_t off = CPU_BP;
            uint8_t color = CPU_BL;
            for (unsigned i = 0; i < CPU_CX; ++i) {
                uint8_t ch = read86(ES_BASE + off++);
                if (CPU_AL & 2) color = read86(ES_BASE + off++);
                pos = emit(page, pos, ch, color, true);
            }
            if (CPU_AL & 1) set_cursor(page, pos);
            return true;
        }
        case 15: CPU_AL = videomode | (RAM[0x487] & 0x80); CPU_AH = columns(); CPU_BH = RAM[0x462]; return true;
        default: return false;
    }
}
