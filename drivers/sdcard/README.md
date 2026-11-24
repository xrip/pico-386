# SD card driver

The production `sdcard` CMake target uses `sdcard_async.c` and
`diskio_async.c`, as listed in `sdcard.cmake`. It links FatFs, Pico SDK SPI,
and DMA support. The FatFs disk layer uses DMA for block transfers.

The M2P2 root build sets SPI0 pins to CS 5, SCK 6, MOSI 7, and MISO 4. The
defaults in [`sdcard_async.h`](sdcard_async.h) apply when a caller does not
override those definitions.

`sdcard.c` is a legacy synchronous implementation and is not part of the
production CMake target. That older file is based on Elehobica's
[`tf_card.c`](https://github.com/elehobica/pico_fatfs_test); the
`drivers/sdcard/LICENSE` file carries its BSD 2-Clause terms. Keep this credit
scoped to the legacy code.
