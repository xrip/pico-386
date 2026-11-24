# RP2350 startup test

This test keeps USB at 48 MHz and does not start HSTX. It prints the flash ID
and status registers, runs the real QE setup, then checks uncached flash
reads at 504 MHz and 1.60 V. It reads clock settings from the SD card without
applying them. A watchdog records the last stage if startup hangs.

```sh
cmake -S tests/flash-startup -B build/flash-startup -G Ninja
cmake --build build/flash-startup
```

A fresh Windows build passed on 2026-11-24. This is compile and link evidence;
it does not confirm a board run.

Use the usual `PICO_SDK_PATH` and ARM toolchain settings. Load
`build/flash-startup/flash_startup.uf2`, then open its USB serial port with
DTR enabled. Send `b` to return to BOOTSEL.

The default sets `PICO_EMBED_XIP_SETUP=0` and uses RP2350 ROM read-mode
detection. `TEST_EMBED_BOOT=ON` changes this SDK setting for a test.
`TEST_SERIAL_BOOT=ON` selects the generic-03h startup experiment, which failed
before USB startup on the tested board; it is not the main firmware setup.

The QE check is the production implementation, so it can set SR2.QE when
clear. It skips status writes when QE is already set. No flash erase is used.
