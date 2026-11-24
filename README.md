# Pico-386

Pico-386 runs DOS games and programs on a Raspberry Pi Pico 2. Its CPU core began with Mike Chambers' Fake86. After many rounds of work on that core, it now emulates an Intel 386SX.

DOS gets a keyboard, mouse, disk drives, color graphics, and stereo sound. Dune II and The Secret of Monkey Island have run on the board. [Game test record](docs/board-records/irq-entry-isolation.md)

**Tested and recommended board:** [Frank M2 v2.05 for Murmulator 2](https://github.com/rh1tech/frank/releases/tag/frank_m2_2_05). The M2P2 firmware uses an RP2350, 16 MB of flash, and 8 MB of QMI PSRAM.

## First boot

1. Build the firmware with the commands below. The default file is `bin/rp2350-arm-s/Release/m2p2-386-C500-P125MHz.uf2`.
2. Hold **BOOTSEL** as you connect the Pico to USB. Copy the UF2 to the USB drive. The board starts when the copy is complete.
3. For a first test, start with the SD card out. The board boots DR DOS from a read-only 360 KB floppy in flash. To use your own boot disk, put its image on the SD card as `/XT/fdd0.img`. The built-in disk is also available if this file is missing.

## What you get

- **One firmware for two screens.** The board checks for HDMI or VGA at boot. HDMI carries picture and sound on one cable. With VGA, the board checks for I2S or PWM sound.
- **DOS disks as files.** Put floppy images in `/XT/fdd0.img` and `/XT/fdd1.img`, and hard disk images in `/XT/hdd.img` and `/XT/hdd2.img`. The SD driver uses DMA for disk transfers.
- **Room for larger programs.** The CPU has 32-bit instructions, protected mode, paging, and virtual 8086 mode. DOS programs can use XMS memory.
- **A desktop build for tests.** The same emulator core runs on Windows and Linux. Pico-386 has its own desktop display code. The desktop build is independent of the Pico SDK.

## Hardware seen by DOS

| Part | Emulated hardware |
| --- | --- |
| CPU and memory | Intel 386SX, x87 floating-point unit, conventional memory, upper memory blocks, HMA and XMS |
| Screen | Text, Hercules, CGA, Tandy, EGA and VGA, including Mode X. [Video modes](VIDEO_MODES.md) |
| Disks | Two floppy drives and two hard disks through BIOS disk services |
| Input | PC keyboard, Microsoft serial mouse on COM1, game-port joystick |
| Sound | PC speaker, Tandy SN76489, AdLib OPL2, Sound Blaster Pro 2, Game Blaster, Covox Speech Thing and Disney Sound Source |
| Music | MPU-401 MIDI and General MIDI synthesis, with an optional GM.DLS sound bank |
| System | 8253 interval timer, 8259 interrupt controller, 8237 DMA controller and CMOS clock |

## Build

With the Pico SDK, Arm GCC, CMake and Ninja in place, run these commands from the project directory. For setup, desktop builds and MIDI options, see the [build guide](BUILDING.md).

```sh
cmake --preset m2p2
cmake --build --preset m2p2
```

## Thanks to the x86 emulation community

Our work on PC peripherals draws on many open-source emulators and chip datasheets. Over the years we tried ideas from so many places that we can no longer trace every part to a single source. The credits below name the sources we can still identify.

Thank you to everyone who takes an interest in x86-compatible hardware and its emulation. Every shared test, note, and piece of code helps others understand how these machines work.

## License and credits

Pico-386 project code is copyright © 2025 xrip under the [MIT License](LICENSE). Third-party code has its own copyright and license terms. The MIT license for the project does not cover the DOS system, programs on the floppy, or third-party sound banks.

| Code | Copyright or credit | License or source notice |
| --- | --- | --- |
| Original Fake86 CPU core | Mike Chambers | [Fake86 project](https://sourceforge.net/projects/fake86/) |
| Raspberry Pi Pico SDK and I2S PIO code | © 2020 Raspberry Pi (Trading) Ltd. and SDK contributors | [SDK license](https://github.com/raspberrypi/pico-sdk/blob/master/LICENSE.TXT), [BSD-3-Clause PIO notice](drivers/audio/audio_i2s.pio) |
| FatFs and its Unicode tables | © 2021 ChaN (FatFs), © 2014 ChaN (Unicode tables) | [FatFs terms](drivers/fatfs/ff.c), [Unicode notice](drivers/fatfs/ffunicode.c) |
| FatFs utilities | © 2021 Carl John Kugler III | [Apache-2.0](drivers/fatfs/f_util.c) |
| Pico SD card bindings | © 2020–2021 Elehobica | [BSD-2-Clause](drivers/sdcard/LICENSE) |
| emu8950, used for OPL2 sound | © 2001–2020 Mitsutaka Okazaki, © 2021–2022 Graham Sanderson | [MIT](src/emu8950/emu8950.c) |
| emu76489, used for Tandy sound | Mitsutaka Okazaki, with Game Gear stereo work by RuRuRu | [Source credit](src/emulator/audio/sn76489.c.inl), [upstream MIT notice](https://github.com/digital-sound-antiques/emu76489/blob/master/LICENSE) |
| XTulator Sound Blaster code | © 2020 Mike Chambers | [GPL-2.0-or-later](src/emulator/audio/sound_blaster.c.inl) |
| Blink floating-point code, via PCulator | © 2022 Justine Alexandra Roberts Tunney | [ISC](src/emulator/fpu.c) |
| I2S audio driver | © 2022 Vincent Mistler | [MIT](drivers/audio/audio.c) |
| HDMI packet code from pico_hdmi and frank-quake2 | fliperama86, rh1tech and DnCraptor | [Source credit](drivers/hstx-vga-hdmi-audio/hstx_video.c), [Unlicense](drivers/hstx-vga-hdmi-audio/LICENSE.pico_hdmi) |
| VGA detection | wbcbz7 | [MIT source credit](drivers/hstx-vga-hdmi-audio/hstx_video.c) |
| Embedded printf | © 2014–2019 Marco Paland | [MIT](src/printf/printf.c) |
| Linux findfirst | © 2011 Mathieu Turcotte | [GPL-3.0-or-later](findfirst/findfirst.c) |

The serial mouse uses the UART model from RP8086. The flash setup follows fhoedemakers' flash_config, and the VGA audio load test follows DnCraptor's pico-spec. [Serial mouse](src/emulator/mouse.c) · [Flash setup](drivers/flash-config/README.md) · [Audio detection](drivers/audio/audio.c)
