# Building the Project

There are two kinds of build:
- the **RP2350 firmware** for the M2P2 board: HSTX HDMI or VGA output, 8 MB onboard QMI PSRAM, HDMI audio for HDMI, and detected I2S or PWM audio for VGA. This is the only supported board configuration;
- the **host build** for Windows or Linux, used for development and tests. The Linux target currently has a compile error noted below.

## Latest build checks

Fresh checks on 2026-11-24:

| Target or check | Result |
|---|---|
| Win32 Release build from a fresh temp directory | Pass |
| RP2350 M2P2 Release build from a fresh temp directory | Pass |
| Linux host build with GCC 13.3 | CMake generation passes; compilation fails in `src/linux-main.cpp` (for example, line 443 assigns `uint32_t *VIDEORAM` to `uint8_t *`). |
| `tests/flash-startup` build | Pass |
| `tests/irq-entry-stress` timer-only build (`TEST_HSTX=OFF`) | Pass |
| `tests/irq-entry-stress` HSTX build (`TEST_HSTX=ON`) | Link fails with undefined `hstx_audio_island`, `hstx_audio_init`, and `port3DA`. |
| Flash timing and QE host tests | Pass |

These results cover fresh builds and host tests only. They do not establish
board stability beyond the separate dated records in `docs/board-records/`.

## Prerequisites

### All Builds
- **CMake** 3.22 or higher, and **Git**.

### Host Build (Linux)
- **C/C++ Compiler**: `gcc`/`g++` (or `clang`). Install with `sudo apt-get install build-essential`.
- **X11 Libraries**: Required for the display. Install with `sudo apt-get install libx11-dev`.

### Host Build (Windows)
- **MinGW GCC** (for example the one that comes with CLion), with the `MinGW Makefiles` generator.

### RP2350 Firmware
- **Pico SDK**, with the `PICO_SDK_PATH` environment variable set to it.
- **ARM GCC toolchain** and **Ninja**. A compiler outside PATH may be given with `-DPICO_TOOLCHAIN_PATH=/path/to/arm-toolchain`.
- A **host C++ compiler** in PATH: the SDK builds `pioasm` and `picotool` with it.

## Host Build (Linux/Windows)

```sh
# Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPICO_PLATFORM=host
cmake --build build -j
# Windows (MinGW)
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DPICO_PLATFORM=host
cmake --build build -j
```

The executable goes to `bin/host/<build type>/`.

## RP2350 Firmware

```sh
cmake --preset m2p2
cmake --build --preset m2p2
```

Output: `bin/rp2350-arm-s/Release/m2p2-386-C500-P125MHz.uf2`. The same firmware supports HSTX HDMI and VGA.

Notes on the M2P2 firmware:
- **Clocks:** SYS runs at 500 MHz (1.60 V). HSTX takes its 126 MHz clock from a separate PLL_USB, so USB and ADC are stopped. USB debugging and USB-triggered BOOTSEL are not available; use the board's BOOTSEL button for the next flash.
- **Sound:** HDMI carries 44.1 kHz stereo in data islands. With VGA, startup probes the load on GPIO9 and GPIO10 to select I2S (DATA 9, BCLK 10, LRCLK 11) or stereo PWM (right 10, left 11). Floating inputs, including an empty socket, can give the I2S signature.
- **PSRAM:** the default QMI clock is `CPU_CLOCK / PSRAM_CLOCK_DIVIDER` (500 MHz / 4 = 125 MHz). At least 8 MB onboard QMI PSRAM is required; the firmware stops at boot without it. Conventional RAM, UMB, HMA and XMS all live in PSRAM.
- **SRAM:** code, lookup tables and palettes use normal SRAM. Scratch X and Y are reserved for separate 4 KB core stacks, with hardware stack-limit checks. The linker rejects other scratch allocations.
- **OPL2:** the OPL2 assembly runs from normal SRAM (2 KB). Both flash and SRAM versions have faulted during HDMI interrupts at 504 MHz; the SRAM change is not a complete fix. See the [OPL2 board record](docs/board-records/hstx-opl-sram.md).
- **Flash:** every boot checks the flash Quad Enable bit before overclocking. If the bit is clear, the startup code sets and verifies it while running from RAM; if it is already set, no status-register write is made. The RP2350 ROM selects the initial flash read mode, then startup selects quad reads before PSRAM or core 1 starts. See [flash setup](drivers/flash-config/README.md). Flash timing keeps the saved boot profile's physical intervals and rate limit when SYS changes. On the measured 150 MHz / 50 MHz boot profile, the derived rate at 500 MHz SYS is about 45.45 MHz; other boot profiles can give a different rate.
- **16 MB flash:** the SDK settings and the linker script use 16 MB flash; the last 4 KB is reserved for CMOS. This requires a board with 16 MB physical flash.
- Earlier 504 MHz checks are in the [historical board record](docs/board-records/hstx-504-board.md). See [VIDEO_MODES.md](VIDEO_MODES.md) for layouts.

### Optional NTSC composite output next to HSTX

`NTSC_TV=ON` adds NTSC composite on GPIO22, sent from a PIO state machine, while
HSTX gives HDMI or VGA as usual. The PIO loop needs an exact 157.5 Mbit/s bit
rate, so SYS runs at 495 MHz (PLL_SYS with REFDIV 2) and PSRAM at 495 / 4 MHz.

```sh
cmake -S . -B build/m2p2-ntsc -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DPICO_PLATFORM=rp2350-arm-s -DNTSC_TV=ON
cmake --build build/m2p2-ntsc
```

Output: `bin/rp2350-arm-s/Release/m2p2-386-C495-P123MHz-NTSC.uf2`. The driver
comes from `rp2040-ntsc-tv-ntsc-pio` (commit c447169). 80-column text and the
two-color 640-pixel modes use 640 luminance samples; other modes use 320 color
pixels. CGA composite modes (0x74, 0x76) send the CGA 640-dot bit stream, one
sample a dot, and the TV makes the artifact colors. The CLion profile
`RP2350 NTSC` builds the same firmware. The NTSC line IRQ is DMA_IRQ_0 on core 1, under the HSTX IRQ. Built on
2026-11-28; not yet checked on a board.

### Optional GM.DLS MIDI on RP2350 and Win32

`MIDI_GM_DLS=ON` selects the GM.DLS wavetable engine.
The default remains the sine/noise MIDI engine. GM.DLS waves are 22,050 Hz,
so MIDI is made at 22,050 Hz and each sample is sent twice to the 44,100 Hz output.
The packed bank must use 22,050 Hz; any other bank is rejected.

Pack your local `gm.dls` with a host C compiler:

```sh
cc -std=c11 -O2 tools/dls_pack.c -lm -o dls_pack
./dls_pack gm.dls src/emulator/audio/gm_bank_22050.bin 22050
```

Then build the firmware with GM.DLS:

```sh
cmake --preset m2p2 -DMIDI_GM_DLS=ON
cmake --build --preset m2p2
```

Output: `bin/rp2350-arm-s/Release/m2p2-386-C500-P125MHz-GMDLS.uf2`.
`MIDI_GM_BANK` can point to another packed v4 bank at 22,050 Hz.
The bank stays in flash; the engine has 32 voices and preserves stereo pan.
MIDI commands go through a queue to the audio core. MPU-401 reset clears its voices.

### MIDI on Win32

The Win32 build (`-DPICO_PLATFORM=host`) has three MIDI choices:

| Options          | MIDI                                   | Output          |
|------------------|----------------------------------------|-----------------|
| (none)           | sine/noise engine                      | `386.exe`       |
| `MIDI_GM_DLS=ON` | GM.DLS wavetable from the packed bank  | `386-GMDLS.exe` |
| `MIDI_WINMM=ON`  | the Windows default MIDI output device | `386-WINMM.exe` |

The files go to `bin/host/<build type>/`. `MIDI_WINMM` and `MIDI_GM_DLS` cannot both be on.

## Technical Notes

### Boot Process
The main firmware sets `PICO_EMBED_XIP_SETUP=0`. The RP2350 ROM detects the initial flash read mode; `main()` then enables QE and sets quad-read timing from RAM before raising the system clock. This note describes the XIP setup setting, not which boot stage runs.

### `PICO_BOARD` Variable
The `m2p2` preset sets `PICO_BOARD=pico2` and `PICO_PLATFORM=rp2350-arm-s` before the SDK is loaded.
