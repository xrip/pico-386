/* Guest-readable VGA ROM. Only the header, vectors and tables live here.
 * Fonts use the system ROM and Pico-386's existing renderer font data. */
#include "emulator/includes/font8x8.h"
#include "emulator/includes/font8x16.h"

static const uint8_t vga_header[0x90] = {
    [0] = 0x55, 0xaa, 20,                 // 20 sectors
    0x0f, 0xff, 6, 0xcb,                  // native option init, RETF
    [0x1e] = 'I','B','M',                  // VGA compatibility signature
    [0x22] = 'P','i','c','o','-','3','8','6',' ','V','G','A',
    [0x40] = 0x0f, 0xff, 0, 0xcf,         // native INT 10h, IRET
    [0x80] = 0xff, 0x67, 0x0f, 0,        // standard mode mask (00-0A, 0D-0E, 10-13)
    [0x87] = 7                           // supported scan-line classes
};

static uint8_t reverse_font_bits(uint8_t value) {
    value = (value >> 4) | (value << 4);
    value = ((value & 0xcc) >> 2) | ((value & 0x33) << 2);
    return ((value & 0xaa) >> 1) | ((value & 0x55) << 1);
}

static uint8_t vga_rom_byte(unsigned off) {
    if (off < sizeof vga_header) return vga_header[off];
    if (off < 0x200) {
        // A system BIOS may checksum the ROM before it calls the entry.
        static bool ready;
        static uint8_t checksum;
        if (off != 0x1ff) return 0;
        if (!ready) {
            for (unsigned i = 0; i < sizeof vga_header; ++i) checksum -= vga_header[i];
            for (unsigned i = 0x200; i < VBIOS_END - VBIOS_START; ++i) checksum -= vga_rom_byte(i);
            ready = true;
        }
        return checksum;
    }
    if (off < 0xa00) {
        unsigned i = off - 0x200;
        return i < 1024 ? BIOS[0x1a6e + i] : reverse_font_bits(font_8x8[i]);
    }
    unsigned i = off - 0x1800;
    if (off < 0x1800) {
        unsigned r = off - 0xa00;
        i = (r / 14) * 16 + 1 + r % 14;
    }
    return reverse_font_bits(font_8x16[i]);
}
