# Repository guide

Pico-386 is an x86 PC emulator with a 386SX CPU core. The RP2350 M2P2 firmware
uses 8 MB of QMI PSRAM and selects HSTX HDMI or VGA output at boot or from
`config.286`. Windows and Linux host builds use the same emulator core.

## Build status

Use [BUILDING.md](BUILDING.md) for commands and dated build results. Clean
Windows Release and M2P2 Release builds pass in the latest checks. Linux CMake generation with GCC
13.3 succeeds, but compilation fails in `src/linux-main.cpp` because of pointer
type errors, including the `VIDEORAM` assignment at line 443. The IRQ stress
test's timer-only build passes; its `TEST_HSTX=ON` build fails at link time.

## Code map

- `src/emulator/cpu.c`, `cpu386.c.inl`, and `protected.c.inl`: CPU decode,
  386 instructions, protected mode, paging, V86 mode, and task switching.
- `src/emulator/memory.c` and `xms.c`: guest memory and XMS services.
- `src/emulator/ports.c` and `src/emulator/video/`: emulated I/O ports and PC
  video hardware.
- `src/pico-main.c`: M2P2 startup, clocks, PSRAM, and device setup.
- `src/win32-main.cpp` and `src/linux-main.cpp`: host entry points and display.
- `drivers/graphics/` and `drivers/hstx-vga-hdmi-audio/`: PC rendering and
  HSTX output.
- `src/emulator/audio/` and `drivers/audio/`: sound-card emulation and output.
- `tests/`: host tests and RP2350 diagnostics. Each test README gives its
  build steps and known limits.

The firmware default is 500 MHz SYS and 125 MHz PSRAM. HDMI audio uses HSTX;
VGA startup selects I2S or PWM audio. USB debug output is disabled in the main
firmware. See [README.md](README.md), [VIDEO_MODES.md](VIDEO_MODES.md), and the
dated board records for feature and hardware details.
