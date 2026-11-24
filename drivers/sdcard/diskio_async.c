#include "ff.h"
#include "diskio.h"
#include "hardware/dma.h"
#include "sdcard_async.h"

static bool dma_ready;

DSTATUS disk_status(BYTE drive) {
    return drive == 0 && dma_ready && sdcard_get_media_state() == SDCARD_MEDIA_READY ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE drive) {
    if (drive || sdcard_init(SDCARD_INIT_ENABLE_CRC) != SDCARD_OK)
        return STA_NOINIT;
    if (sdcard_set_speed_mode(SDCARD_SPEED_HIGH, NULL) == SDCARD_OK)
        sdcard_set_bus_baud_rate(50000000u, NULL);
    if (!dma_ready) {
        sdcard_async_config_t config = sdcard_async_get_default_config();
        config.tx_dma_channel = dma_claim_unused_channel(false);
        config.rx_dma_channel = dma_claim_unused_channel(false);
        config.blocking_transport = SDCARD_BLOCKING_TRANSPORT_DMA;
        if (sdcard_async_init(&config) != SDCARD_OK) {
            if (config.tx_dma_channel >= 0) dma_channel_unclaim(config.tx_dma_channel);
            if (config.rx_dma_channel >= 0) dma_channel_unclaim(config.rx_dma_channel);
            return STA_NOINIT;
        }
        dma_ready = true;
    }
    return 0;
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) {
    if (disk_status(drive)) return RES_NOTRDY;
    return sdcard_read_blocks(sector, buffer, count) == SDCARD_OK ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count) {
    if (disk_status(drive)) return RES_NOTRDY;
    return sdcard_write_blocks(sector, buffer, count) == SDCARD_OK ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer) {
    if (disk_status(drive)) return RES_NOTRDY;
    switch (command) {
    case CTRL_SYNC:
        return sdcard_sync() == SDCARD_OK ? RES_OK : RES_ERROR;
    case GET_SECTOR_SIZE:
        *(WORD *)buffer = SDCARD_BLOCK_SIZE;
        return RES_OK;
    case GET_SECTOR_COUNT: {
        sdcard_csd_t csd;
        if (sdcard_read_csd(&csd) != SDCARD_OK) return RES_ERROR;
        *(LBA_t *)buffer = csd.sector_count;
        return RES_OK;
    }
    case GET_BLOCK_SIZE:
        return sdcard_get_erase_block_size(buffer) == SDCARD_OK ? RES_OK : RES_ERROR;
    case CTRL_TRIM: {
        const LBA_t *range = buffer;
        return sdcard_erase_blocks(range[0], range[1]) == SDCARD_OK ? RES_OK : RES_ERROR;
    }
    default:
        return RES_PARERR;
    }
}
