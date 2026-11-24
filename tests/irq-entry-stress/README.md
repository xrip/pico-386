# RP2350 IRQ entry stress test

This test runs fixed math, fill, copy, and lookup loops on both cores. Core 1
uses a 32 us timer IRQ by default, or the HSTX DMA IRQ when `TEST_HSTX=ON`.
There is no PC or OPL code. HSTX sends sync and black packets; it does not show
a game screen. Both cores check their results against checksums made at the
boot clock.

## Build

Set `PICO_SDK_PATH` to a local Pico SDK. Make the Arm GCC tools available on
`PATH`, or set `PICO_TOOLCHAIN_PATH` to their toolchain root.

```powershell
$env:PICO_SDK_PATH = 'C:/path/to/pico-sdk'
cmake -S tests/irq-entry-stress -B build/irq-entry-timer -G Ninja `
    -DPICO_TOOLCHAIN_PATH='C:/path/to/arm-toolchain' `
    -DCMAKE_BUILD_TYPE=Release -DTEST_CPU_MHZ=504 -DTEST_FLASH_MHZ=84 `
    -DTEST_HSTX=OFF
cmake --build build/irq-entry-timer --parallel
```

For an HSTX build, use a separate folder (`build/irq-entry-hstx`) and set
`-DTEST_HSTX=ON`. Its current Windows build fails at link time with undefined
`hstx_audio_island`, `hstx_audio_init`, and `port3DA`. A
fresh timer-only Windows build passed on 2026-11-24. The older HSTX results
below came from saved images; a clean checkout cannot rebuild that image until
the current HSTX link errors are fixed.

## Options and output

| CMake option | Default | Effect |
|---|---:|---|
| `TEST_CPU_MHZ` | `504` | CPU clock: 504 or 252 MHz. |
| `TEST_FLASH_MHZ` | `84` | Flash clock: 84 or 42 MHz. RX delay is 6 at CPU504 and 3 at CPU252. |
| `TEST_HSTX` | `OFF` | Use HSTX DMA IRQ1 instead of timer IRQ2. HSTX is 126 MHz; USB stays at 48 MHz. |
| `TEST_CORE0_RAM` | `OFF` | Move core 0 work code and lookup table to SRAM. |
| `TEST_PROBE_RAM` | `OFF` | Move core 1 work code to SRAM; its lookup stays in flash. |
| `TEST_PSRAM` | `OFF` | Put core 0 lookup data in QMI PSRAM. Requires `TEST_CORE0_RAM=ON`. |
| `TEST_PSRAM_MHZ` | `126` | PSRAM clock: 126 or 84 MHz. |

For HSTX with core 0 SRAM work, add `-DTEST_HSTX=ON -DTEST_CORE0_RAM=ON`.
PSRAM tests also set `-DTEST_PSRAM=ON` and `-DTEST_PSRAM_MHZ=126` or `84` in
separate build folders. The setup checks all 16,384 lookup words before core 1
starts; `verify_bad` must be zero.

The ELF and UF2 use names like
`irq-entry-<CPU>-f<flash>-<flash|ram>[-hstx][-c0ram][-psram<rate>]`. Use
`arm-none-eabi-nm -S <test.elf>` to check function and data addresses, then keep
the UF2 SHA256 with the serial log. Flash addresses start with `0x100`; SRAM
addresses start with `0x200`. The IRQ handler must be in SRAM.

USB status prints each second. Send `v` for status or `b` for ROM BOOTSEL; a
1200-baud USB serial open also requests BOOTSEL. `c0=` and `c1=` show each
worker's rounds, checksum, and mismatch count. `irq` counts timer IRQs only;
HSTX uses a separate `frames/restarts/run/phase` line. A fixed frame count with
`run=1` does not prove the IRQ is working. A core 1 fault is recorded by
`src/fault_trace.c`.

## Historical failure and exact image

On 2026-11-16, a CPU504/flash84 HSTX run with core 0 SRAM code/data, core 1
flash code/data, and no PSRAM failed at 15,629 frames after 1,523,594 core 1
rounds. It had no checksum errors or HSTX restarts. The fault was at
`PC=1000011e` (`isr_svcall`, `BKPT`), with `LR=ffffffe9`, `CFSR=00000000`,
`HFSR=80000000`, and `xPSR=8900001b`. See the tracked
[IRQ isolation record](../../docs/board-records/irq-entry-isolation.md).

The failed UF2 SHA256 was:

```text
D67E4F2F6D2308C9EE4C387C9EAA8BAE9F8B964261885DFD3837110C22E9B5EA
```

The exact UF2, ELF, source snapshot, `fault.log`, and hash list were kept in the
ignored local folder `build/irq-entry-hstx-c0ram-baseline/`. They are not part
of a clean checkout. The old frozen-source rebuild also depends on local SDK
and driver copies; do not treat its command as a clean-checkout recipe.

The CPU504/flash42 control later failed at 33,149 frames and 1,739,579 core 1
rounds (`PC=30000514`, `CFSR=00000100`, `HFSR=40000000`).

The first no-PSRAM-access tests left GPIO8 unconfigured, so they did not prove
that the physical PSRAM chip was inactive on the shared bus. A later control
explicitly held GPIO8 high and completed a 600-second observation with no
checksum errors, faults, or restarts. The current test source also holds GPIO8
high before flash setup; the PSRAM option later assigns it to QMI CS1. These
results show that chip-select setup matters in this test, but do not prove it
was the only cause of earlier game faults. See the same
[tracked isolation record](../../docs/board-records/irq-entry-isolation.md).
