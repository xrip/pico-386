#include "sdcard_async.h"

#include <limits.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "pico.h"
#include "pico/critical_section.h"
#include "pico/stdlib.h"

#ifdef SDCARD_PIO
#error "The first standalone async driver supports the hardware SPI backend only"
#endif

/* -------------------------------------------------------------------------- */
/* Protocol constants                                                        */
/* -------------------------------------------------------------------------- */

/* MMC/SD command indices. The high bit marks an application-specific command. */
#define CMD0 (0)           /* GO_IDLE_STATE */
#define CMD1 (1)           /* SEND_OP_COND (MMC) */
#define CMD6 (6)           /* SWITCH_FUNC (SDC) */
#define ACMD41 (0x80 + 41) /* SEND_OP_COND (SDC) */
#define CMD8 (8)           /* SEND_IF_COND */
#define CMD9 (9)           /* SEND_CSD */
#define CMD10 (10)         /* SEND_CID */
#define CMD12 (12)         /* STOP_TRANSMISSION */
#define CMD13 (13)         /* SEND_STATUS */
#define ACMD13 (0x80 + 13) /* SD_STATUS (SDC) */
#define CMD16 (16)         /* SET_BLOCKLEN */
#define CMD17 (17)         /* READ_SINGLE_BLOCK */
#define CMD18 (18)         /* READ_MULTIPLE_BLOCK */
#define CMD23 (23)         /* SET_BLOCK_COUNT (MMC) */
#define ACMD23 (0x80 + 23) /* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24 (24)         /* WRITE_BLOCK */
#define CMD25 (25)         /* WRITE_MULTIPLE_BLOCK */
#define CMD32 (32)         /* ERASE_ER_BLK_START */
#define CMD33 (33)         /* ERASE_ER_BLK_END */
#define CMD38 (38)         /* ERASE */
#define CMD55 (55)         /* APP_CMD */
#define CMD58 (58)         /* READ_OCR */
#define CMD59 (59)         /* CRC_ON_OFF */
#define ACMD51 (0x80 + 51) /* SEND_SCR (SDC) */

/* Internal card type flags. */
#define CT_MMC 0x01              /* MMC ver 3 */
#define CT_SD1 0x02              /* SD ver 1 */
#define CT_SD2 0x04              /* SD ver 2 */
#define CT_SDC (CT_SD1 | CT_SD2) /* SD */
#define CT_BLOCK 0x08            /* Block addressing */

#ifndef SDCARD_ASYNC_TASK_TRANSITION_BUDGET
#define SDCARD_ASYNC_TASK_TRANSITION_BUDGET 8u
#endif

#ifndef SDCARD_ASYNC_TASK_SPI_POLL_BUDGET
#define SDCARD_ASYNC_TASK_SPI_POLL_BUDGET 8u
#endif

typedef struct {
    uint32_t actual_baud_rate;
    bool active;
} sdcard_bus_state_t;

typedef struct {
    uint64_t sector_count;
    uint32_t erase_sector_size;
    uint32_t default_max_baud_rate;
    sdcard_speed_mode_t speed_mode;
    uint16_t command_classes;
    uint8_t type; /**< Internal CT_* flags. */
    bool initialized;
    bool write_protected;
} sdcard_card_state_t;

typedef struct {
    sdcard_init_flags_t init_flags;
} sdcard_config_state_t;

typedef struct {
    sdcard_media_state_t state;
    uint32_t generation;
    uint8_t initialized_cid[16];
    bool initialized_cid_valid;
} sdcard_media_tracker_t;

typedef enum {
    ASYNC_OPERATION_READ = 0,
    ASYNC_OPERATION_WRITE,
} sdcard_async_operation_t;

typedef enum {
    ASYNC_PHASE_IDLE = 0,
    ASYNC_PHASE_QUEUED,
    ASYNC_PHASE_PRECHECK_STATUS_WAIT_READY,
    ASYNC_PHASE_PRECHECK_CID_WAIT_READY,
    ASYNC_PHASE_PRECHECK_CID_WAIT_TOKEN,
    ASYNC_PHASE_COMMAND_WAIT_READY,
    ASYNC_PHASE_READ_WAIT_TOKEN,
    ASYNC_PHASE_READ_DMA,
    ASYNC_PHASE_READ_DRAIN,
    ASYNC_PHASE_WRITE_WAIT_READY,
    ASYNC_PHASE_WRITE_DMA,
    ASYNC_PHASE_WRITE_DRAIN,
    ASYNC_PHASE_WRITE_RESPONSE,
    ASYNC_PHASE_WRITE_WAIT_BUSY,
    ASYNC_PHASE_WRITE_WAIT_FINAL_BUSY,
    ASYNC_PHASE_PROGRESS_PENDING,
    ASYNC_PHASE_IRQ_COMPLETION_PENDING,
    ASYNC_PHASE_COMPLETION_PENDING,
} sdcard_async_phase_t;

typedef struct {
    uint64_t deadline_us;
    uint8_t *read_buffer;
    const uint8_t *write_buffer;
    sdcard_async_callback_t callback;
    void *user_data;
    uint32_t lba;
    size_t count;
    size_t transferred;
    size_t progress_interval_blocks;
    size_t read_dma_block_index;
    sdcard_result_t result;
    sdcard_async_operation_t operation;
    uint16_t data_crc;
    uint16_t read_pending_crc;
    uint16_t next_data_crc;
    bool deadline_enabled;
    bool multi_block;
    bool transfer_started;
    bool dma_active;
    bool read_pending_crc_valid;
    bool read_progress_pending;
    bool next_data_crc_valid;
    bool blocking_facade;
} sdcard_async_request_t;

typedef struct {
    sdcard_async_request_t request;
    sdcard_async_config_t config;
#if SDCARD_ASYNC_ENABLE_STATS
    uint64_t dma_irq_payloads;
    uint64_t dma_irq_latency_us_total;
#endif
    sdcard_result_t irq_deferred_result;
    sdcard_async_phase_t phase;
    sdcard_async_phase_t progress_resume_phase;
    sdcard_async_event_flags_t pending_callback_events;
    volatile uint32_t dma_irq_events;
#if SDCARD_ASYNC_ENABLE_STATS
    volatile uint32_t dma_irq_event_time_us;
#endif
#if SDCARD_ASYNC_ENABLE_STATS
    uint32_t dma_irq_latency_us_max;
    uint32_t dma_errors;
#endif
    volatile uint32_t task_gate;
    uint owner_core;
    uint8_t probe_candidate[16];
    uint8_t previous_mismatch[16];
    uint8_t tx_fill;
    uint8_t rx_sink;
    uint8_t probe_attempts;
    bool initialized;
    bool cancel_requested;
    bool have_mismatch;
    bool have_successful_probe;
    bool dma_irq_handler_installed;
    bool task_from_irq_active;
} sdcard_async_state_t;

typedef enum {
    SDCARD_OWNER_UNINITIALIZED = 0,
    SDCARD_OWNER_IDLE,
    SDCARD_OWNER_BLOCKING,
    SDCARD_OWNER_ASYNC,
} sdcard_owner_t;

typedef struct {
    critical_section_t lock;
    sdcard_owner_t owner;
} sdcard_arbitration_state_t;

typedef struct {
    sdcard_bus_state_t bus;
    sdcard_card_state_t card;
    sdcard_config_state_t config;
    sdcard_media_tracker_t media;
    sdcard_arbitration_state_t arbitration;
    sdcard_async_state_t async;
} sdcard_driver_state_t;

static sdcard_driver_state_t driver_state;

#define ASYNC_TASK_GATE_RUNNING (1u << 0)
#define ASYNC_TASK_GATE_PENDING (1u << 1)

static bool blocking_dma_transport_enabled(void);
static sdcard_result_t blocking_dma_transfer(sdcard_async_operation_t operation, uint32_t lba, uint8_t *read_buffer,
                                             const uint8_t *write_buffer, size_t count);

static uint32_t async_dma_channel_mask(void) {
    return (1u << (uint)driver_state.async.config.tx_dma_channel) | (1u << (uint)driver_state.async.config.rx_dma_channel);
}

static void async_dma_irq_handler(void) {
    if (!driver_state.async.initialized || !driver_state.async.config.use_dma_irq)
        return;

    const uint irq_index = driver_state.async.config.dma_irq_index;
    const uint32_t channel_mask = async_dma_channel_mask();
    const uint32_t pending = dma_hw->irq_ctrl[irq_index].ints & channel_mask;
    if (pending == 0)
        return;

    /* This is a shared line: acknowledge only this driver's channels. */
    dma_hw->irq_ctrl[irq_index].ints = pending;
    const uint32_t previous_events = __atomic_fetch_or(&driver_state.async.dma_irq_events, pending, __ATOMIC_RELEASE);
#if SDCARD_ASYNC_ENABLE_STATS
    if ((previous_events & channel_mask) == 0) {
        __atomic_store_n(&driver_state.async.dma_irq_event_time_us, time_us_32(), __ATOMIC_RELEASE);
    }
#else
    (void)previous_events;
#endif
    __sev();
}

/* -------------------------------------------------------------------------- */
/* Driver state and arbitration                                              */
/* -------------------------------------------------------------------------- */

static inline uint32_t clock_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void clear_card_state(void) {
    driver_state.card = (sdcard_card_state_t){
        .speed_mode = SDCARD_SPEED_DEFAULT,
    };
}

static sdcard_result_t ready_state_result(void) {
    if (driver_state.media.state == SDCARD_MEDIA_CHANGED || driver_state.media.state == SDCARD_MEDIA_AVAILABLE) {
        return SDCARD_ERROR_MEDIA_CHANGED;
    }
    return driver_state.card.initialized && driver_state.media.state == SDCARD_MEDIA_READY ? SDCARD_OK : SDCARD_ERROR_NOT_READY;
}

/* sdcard_init() is the single-threaded lifetime boundary for this lock. */
static void arbitration_init_once(void) {
    if (driver_state.arbitration.owner != SDCARD_OWNER_UNINITIALIZED)
        return;
    critical_section_init(&driver_state.arbitration.lock);
    driver_state.arbitration.owner = SDCARD_OWNER_IDLE;
}

static sdcard_result_t blocking_enter(void) {
    if (driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED) {
        return SDCARD_ERROR_NOT_READY;
    }
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    if (driver_state.arbitration.owner != SDCARD_OWNER_IDLE) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_BUSY;
    }
    driver_state.arbitration.owner = SDCARD_OWNER_BLOCKING;
    critical_section_exit(&driver_state.arbitration.lock);
    return SDCARD_OK;
}

static void blocking_exit(void) {
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    hard_assert(driver_state.arbitration.owner == SDCARD_OWNER_BLOCKING);
    driver_state.arbitration.owner = SDCARD_OWNER_IDLE;
    critical_section_exit(&driver_state.arbitration.lock);
    __sev();
}

static bool async_is_busy(void) {
    if (driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED)
        return false;
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    const bool busy = driver_state.async.initialized && driver_state.arbitration.owner == SDCARD_OWNER_ASYNC;
    critical_section_exit(&driver_state.arbitration.lock);
    return busy;
}

static void invalidate_media(sdcard_media_event_t event) {
    if (driver_state.media.state == SDCARD_MEDIA_READY)
        ++driver_state.media.generation;
    clear_card_state();
    driver_state.media.state = event == SDCARD_MEDIA_EVENT_AVAILABLE ? SDCARD_MEDIA_AVAILABLE : SDCARD_MEDIA_CHANGED;
}

/* -------------------------------------------------------------------------- */
/* SPI controls                                                               */
/* -------------------------------------------------------------------------- */

static inline void cs_select(uint cs_pin) {
    /* Keep CS setup/hold delays explicit; they are part of the SPI timing contract. */
    asm volatile("nop \n nop \n nop");
    gpio_put(cs_pin, 0);
    asm volatile("nop \n nop \n nop");
}

static inline void cs_deselect(uint cs_pin) {
    asm volatile("nop \n nop \n nop");
    gpio_put(cs_pin, 1);
    asm volatile("nop \n nop \n nop");
}

static uint32_t set_spi_baud_rate(uint32_t baud_rate) {
    driver_state.bus.actual_baud_rate = spi_set_baudrate(SDCARD_SPI_BUS, baud_rate);
    return driver_state.bus.actual_baud_rate;
}

static void spi_set_initial_baud_rate(void) {
    set_spi_baud_rate(SDCARD_SPI_INITIAL_BAUD_RATE);
}

static void spi_set_data_baud_rate(void) {
    set_spi_baud_rate(SDCARD_SPI_BAUD_RATE);
}

static void spi_cs_high(void) {
    cs_deselect(SDCARD_PIN_SPI0_CS);
}

static void spi_cs_low(void) {
    cs_select(SDCARD_PIN_SPI0_CS);
}

/* Initialize the SPI pins and peripheral. */
static void init_spi(void) {
    /* MISO needs a pull-up; the card actively drives it only while selected. */
    gpio_init(SDCARD_PIN_SPI0_SCK);
    gpio_init(SDCARD_PIN_SPI0_MISO);
    gpio_pull_up(SDCARD_PIN_SPI0_MISO);
    gpio_init(SDCARD_PIN_SPI0_MOSI);
    gpio_pull_up(SDCARD_PIN_SPI0_MOSI);
    gpio_init(SDCARD_PIN_SPI0_CS);
    gpio_set_dir(SDCARD_PIN_SPI0_CS, GPIO_OUT);

    /* Keep chip select inactive until a command explicitly selects the card. */
    spi_cs_high();

    gpio_set_function(SDCARD_PIN_SPI0_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SDCARD_PIN_SPI0_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SDCARD_PIN_SPI0_MOSI, GPIO_FUNC_SPI);

    driver_state.bus.actual_baud_rate = spi_init(SDCARD_SPI_BUS, SDCARD_SPI_INITIAL_BAUD_RATE);

    spi_set_format(SDCARD_SPI_BUS, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    driver_state.bus.active = true;
}

static void release_spi(void) {
    if (!driver_state.bus.active)
        return;
    spi_cs_high();
    spi_deinit(SDCARD_SPI_BUS);
    gpio_deinit(SDCARD_PIN_SPI0_CS);
    gpio_deinit(SDCARD_PIN_SPI0_SCK);
    gpio_deinit(SDCARD_PIN_SPI0_MOSI);
    gpio_deinit(SDCARD_PIN_SPI0_MISO);
    driver_state.bus.active = false;
    driver_state.bus.actual_baud_rate = 0;
}

/* Transfer one byte while keeping the SPI peripheral selected. */
static uint8_t spi_transfer_byte(uint8_t data) {
    uint8_t *buffer = &data;
    spi_write_read_blocking(SDCARD_SPI_BUS, buffer, buffer, 1);
    return *buffer;
}

/* Receive bytes by clocking dummy 0xFF values. */
static void spi_receive_bytes(uint8_t *buffer, size_t length) {
    spi_read_blocking(SDCARD_SPI_BUS, 0xff, buffer, length);
}

static bool data_crc_enabled(void) {
    return (driver_state.config.init_flags & SDCARD_INIT_ENABLE_CRC) != 0;
}

static bool async_crc_pipeline_enabled(void) {
#if SDCARD_ASYNC_CRC_PIPELINE
    return data_crc_enabled();
#else
    return false;
#endif
}

/* -------------------------------------------------------------------------- */
/* CRC helpers                                                                */
/* -------------------------------------------------------------------------- */

/* CRC7 transition table indexed by (previous_crc << 1) ^ input_byte. */
static const uint8_t crc7_table[256] = {
    0x00, 0x09, 0x12, 0x1B, 0x24, 0x2D, 0x36, 0x3F, 0x48, 0x41, 0x5A, 0x53, 0x6C, 0x65, 0x7E, 0x77, 0x19, 0x10, 0x0B, 0x02, 0x3D, 0x34,
    0x2F, 0x26, 0x51, 0x58, 0x43, 0x4A, 0x75, 0x7C, 0x67, 0x6E, 0x32, 0x3B, 0x20, 0x29, 0x16, 0x1F, 0x04, 0x0D, 0x7A, 0x73, 0x68, 0x61,
    0x5E, 0x57, 0x4C, 0x45, 0x2B, 0x22, 0x39, 0x30, 0x0F, 0x06, 0x1D, 0x14, 0x63, 0x6A, 0x71, 0x78, 0x47, 0x4E, 0x55, 0x5C, 0x64, 0x6D,
    0x76, 0x7F, 0x40, 0x49, 0x52, 0x5B, 0x2C, 0x25, 0x3E, 0x37, 0x08, 0x01, 0x1A, 0x13, 0x7D, 0x74, 0x6F, 0x66, 0x59, 0x50, 0x4B, 0x42,
    0x35, 0x3C, 0x27, 0x2E, 0x11, 0x18, 0x03, 0x0A, 0x56, 0x5F, 0x44, 0x4D, 0x72, 0x7B, 0x60, 0x69, 0x1E, 0x17, 0x0C, 0x05, 0x3A, 0x33,
    0x28, 0x21, 0x4F, 0x46, 0x5D, 0x54, 0x6B, 0x62, 0x79, 0x70, 0x07, 0x0E, 0x15, 0x1C, 0x23, 0x2A, 0x31, 0x38, 0x41, 0x48, 0x53, 0x5A,
    0x65, 0x6C, 0x77, 0x7E, 0x09, 0x00, 0x1B, 0x12, 0x2D, 0x24, 0x3F, 0x36, 0x58, 0x51, 0x4A, 0x43, 0x7C, 0x75, 0x6E, 0x67, 0x10, 0x19,
    0x02, 0x0B, 0x34, 0x3D, 0x26, 0x2F, 0x73, 0x7A, 0x61, 0x68, 0x57, 0x5E, 0x45, 0x4C, 0x3B, 0x32, 0x29, 0x20, 0x1F, 0x16, 0x0D, 0x04,
    0x6A, 0x63, 0x78, 0x71, 0x4E, 0x47, 0x5C, 0x55, 0x22, 0x2B, 0x30, 0x39, 0x06, 0x0F, 0x14, 0x1D, 0x25, 0x2C, 0x37, 0x3E, 0x01, 0x08,
    0x13, 0x1A, 0x6D, 0x64, 0x7F, 0x76, 0x49, 0x40, 0x5B, 0x52, 0x3C, 0x35, 0x2E, 0x27, 0x18, 0x11, 0x0A, 0x03, 0x74, 0x7D, 0x66, 0x6F,
    0x50, 0x59, 0x42, 0x4B, 0x17, 0x1E, 0x05, 0x0C, 0x33, 0x3A, 0x21, 0x28, 0x5F, 0x56, 0x4D, 0x44, 0x7B, 0x72, 0x69, 0x60, 0x0E, 0x07,
    0x1C, 0x15, 0x2A, 0x23, 0x38, 0x31, 0x46, 0x4F, 0x54, 0x5D, 0x62, 0x6B, 0x70, 0x79,
};

static uint8_t crc7_command(uint8_t command, uint32_t argument) {
    const uint8_t packet[5] = {
        (uint8_t)(0x40u | (command & 0x3Fu)),
        (uint8_t)(argument >> 24),
        (uint8_t)(argument >> 16),
        (uint8_t)(argument >> 8),
        (uint8_t)argument,
    };
    uint8_t crc = 0;
    for (size_t byte = 0; byte < sizeof(packet); ++byte) {
        crc = crc7_table[(uint8_t)((crc << 1) ^ packet[byte])];
    }
    return (uint8_t)((crc << 1) | 1u);
}

#if SDCARD_CRC16_IMPL == SDCARD_CRC16_IMPL_NIBBLE
static const uint16_t crc16_nibble_table[16] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7, 0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
};
#elif SDCARD_CRC16_IMPL == SDCARD_CRC16_IMPL_BYTE
static const uint16_t crc16_byte_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7, 0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6, 0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485, 0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4, 0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823, 0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12, 0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41, 0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70, 0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F, 0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E, 0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D, 0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C, 0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB, 0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A, 0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9, 0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8, 0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};
#endif

static uint16_t crc16_ccitt(const uint8_t *buffer, size_t length) {
    uint16_t crc = 0;
    for (size_t index = 0; index < length; ++index) {
#if SDCARD_CRC16_IMPL == SDCARD_CRC16_IMPL_BYTE
        crc = (uint16_t)(crc16_byte_table[((crc >> 8) ^ buffer[index]) & 0xFFu] ^ (crc << 8));
#elif SDCARD_CRC16_IMPL == SDCARD_CRC16_IMPL_NIBBLE
        crc ^= (uint16_t)buffer[index] << 8;
        crc = (uint16_t)((crc << 4) ^ crc16_nibble_table[crc >> 12]);
        crc = (uint16_t)((crc << 4) ^ crc16_nibble_table[crc >> 12]);
#else
        crc ^= (uint16_t)buffer[index] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0));
        }
#endif
    }
    return crc;
}

#ifdef SDCARD_CRC_TEST_HOOK
static uint32_t crc_test_packets_before_error;
static bool crc_test_error_armed;

void sdcard_crc_test_corrupt_data_crc_after(uint32_t successful_packets) {
    crc_test_packets_before_error = successful_packets;
    crc_test_error_armed = true;
}

static bool crc_test_take_data_corruption(void) {
    if (!crc_test_error_armed)
        return false;
    if (crc_test_packets_before_error != 0) {
        --crc_test_packets_before_error;
        return false;
    }
    crc_test_error_armed = false;
    return true;
}

bool sdcard_crc_test_vectors(void) {
    static const uint8_t text[] = "123456789";
    uint8_t sequence[256];
    for (unsigned index = 0; index < sizeof(sequence); ++index) {
        sequence[index] = (uint8_t)index;
    }
    return crc7_command(CMD0, 0) == 0x95u && crc7_command(CMD8, 0x1AAu) == 0x87u && crc7_command(CMD55, 0) == 0x65u &&
           crc7_command(41, 0x40000000u) == 0x77u && crc16_ccitt(text, sizeof(text) - 1u) == 0x31C3u &&
           crc16_ccitt(sequence, sizeof(sequence)) == 0x7E55u;
}
#else
static bool crc_test_take_data_corruption(void) {
    return false;
}
#endif

/* Wait until the card releases its busy signal or the timeout expires. */
static bool spi_wait_card_ready(uint32_t timeout_ms) {
    uint8_t response;
    const uint32_t start = clock_millis();
    do {
        response = spi_transfer_byte(0xFF);
    } while (response != 0xFF && (uint32_t)(clock_millis() - start) < timeout_ms);
    return response == 0xFF;
}

/* Release chip select and provide the trailing clock required by SPI cards. */
static void spi_deselect_card(void) {
    spi_cs_high();
    spi_transfer_byte(0xFF);
}

/* Assert chip select and wait until the card is ready for a command. */
static bool spi_select_card(void) {
    spi_cs_low();
    spi_transfer_byte(0xFF);
    if (spi_wait_card_ready(500)) {
        return true;
    }
    spi_deselect_card();
    return false;
}

/* Receive one token-delimited data packet and optionally validate its CRC16. */
static sdcard_result_t spi_receive_data_block(uint8_t *buffer, size_t length) {
    uint8_t token;
    const uint32_t timeout = 200;
    const uint32_t start = clock_millis();
    do {
        token = spi_transfer_byte(0xFF);
    } while (token == 0xFF && (uint32_t)(clock_millis() - start) < timeout);
    if (token == 0xFF)
        return SDCARD_ERROR_TIMEOUT;
    if (token != 0xFE)
        return SDCARD_ERROR_DATA_TOKEN;

    spi_receive_bytes(buffer, length);
    uint16_t received_crc = (uint16_t)spi_transfer_byte(0xFF) << 8;
    received_crc = (uint16_t)(received_crc | spi_transfer_byte(0xFF));
    if (crc_test_take_data_corruption())
        received_crc ^= 1u;
    if (data_crc_enabled() && received_crc != crc16_ccitt(buffer, length)) {
        return SDCARD_ERROR_CRC;
    }

    return SDCARD_OK;
}

/* Send a command packet and return the raw R1 response. */
static uint8_t spi_send_command(uint8_t command, uint32_t argument) {
    uint8_t response;
    uint8_t wait_count;

    if (command & 0x80) {
        command &= 0x7F;
        response = spi_send_command(CMD55, 0);
        if (response > 1) {
            return response;
        }
    }

    /* CMD12 terminates a multi-block transfer while the card is still selected. */
    if (command != CMD12) {
        spi_deselect_card();
        if (!spi_select_card()) {
            return 0xFF;
        }
    }

    spi_transfer_byte((uint8_t)(0x40u | command));
    spi_transfer_byte((uint8_t)(argument >> 24));
    spi_transfer_byte((uint8_t)(argument >> 16));
    spi_transfer_byte((uint8_t)(argument >> 8));
    spi_transfer_byte((uint8_t)argument);
    const uint8_t crc = data_crc_enabled() ? crc7_command(command, argument) : (command == CMD0 ? 0x95 : command == CMD8 ? 0x87 : 0x01);
    spi_transfer_byte(crc);

    if (command == CMD12) {
        spi_transfer_byte(0xFF);
    }
    wait_count = 10;
    do {
        response = spi_transfer_byte(0xFF);
    } while ((response & 0x80u) && --wait_count != 0);

    return response;
}

static sdcard_result_t command_result(uint8_t response) {
    if (response == 0xFF)
        return SDCARD_ERROR_TIMEOUT;
    if (response & 0x08u)
        return SDCARD_ERROR_CRC;
    if (response & (0x20u | 0x40u))
        return SDCARD_ERROR_OUT_OF_RANGE;
    if (response & 0x04u)
        return SDCARD_ERROR_UNSUPPORTED;
    if (response != 0)
        return SDCARD_ERROR_COMMAND_REJECTED;
    return SDCARD_OK;
}

static sdcard_result_t sync_internal(void) {
    const bool ready = spi_select_card();
    spi_deselect_card();
    return ready ? SDCARD_OK : SDCARD_ERROR_TIMEOUT;
}

static sdcard_result_t media_probe_once(uint8_t cid[16]) {
    const uint8_t r1 = spi_send_command(CMD13, 0);
    if (r1 == 0xFF || (r1 & 0x80u)) {
        spi_deselect_card();
        return SDCARD_ERROR_TIMEOUT;
    }
    (void)spi_transfer_byte(0xFF); /* Second R2 byte. */
    spi_deselect_card();
    if (r1 & 0x01u)
        return SDCARD_ERROR_NOT_READY;

    sdcard_result_t result = command_result(spi_send_command(CMD10, 0));
    if (result == SDCARD_OK)
        result = spi_receive_data_block(cid, 16);
    spi_deselect_card();
    return result;
}

static sdcard_result_t check_initialized_media_internal(sdcard_media_event_t *event) {
    uint8_t previous_mismatch[16];
    bool have_mismatch = false;
    bool have_successful_probe = false;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        sdcard_result_t result = sync_internal();
        uint8_t candidate[16];
        if (result == SDCARD_OK)
            result = media_probe_once(candidate);
        if (result != SDCARD_OK) {
            sleep_ms(1);
            continue;
        }

        have_successful_probe = true;
        if (driver_state.media.initialized_cid_valid && memcmp(candidate, driver_state.media.initialized_cid, 16) == 0) {
            return SDCARD_OK;
        }
        if (have_mismatch && memcmp(candidate, previous_mismatch, 16) == 0) {
            invalidate_media(SDCARD_MEDIA_EVENT_CHANGED);
            if (event != NULL)
                *event = SDCARD_MEDIA_EVENT_CHANGED;
            return SDCARD_ERROR_MEDIA_CHANGED;
        }
        memcpy(previous_mismatch, candidate, 16);
        have_mismatch = true;
        sleep_ms(1);
    }

    if (!have_successful_probe) {
        invalidate_media(SDCARD_MEDIA_EVENT_REMOVED);
        if (event != NULL)
            *event = SDCARD_MEDIA_EVENT_REMOVED;
        return SDCARD_ERROR_MEDIA_CHANGED;
    }
    return SDCARD_ERROR_IO;
}

static sdcard_result_t pre_operation_internal(void) {
    sdcard_result_t result = ready_state_result();
    if (result != SDCARD_OK)
        return result;
    if (driver_state.config.init_flags & SDCARD_INIT_CHECK_MEDIA_BEFORE_IO) {
        return check_initialized_media_internal(NULL);
    }
    return sync_internal();
}

static sdcard_result_t validate_range(uint32_t lba, size_t count) {
    if (count == 0)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    if (driver_state.card.sector_count == 0 || (uint64_t)lba >= driver_state.card.sector_count ||
        (uint64_t)count > driver_state.card.sector_count - lba) {
        return SDCARD_ERROR_OUT_OF_RANGE;
    }
    return SDCARD_OK;
}

static uint32_t extract_bits(const uint8_t raw[16], unsigned msb, unsigned lsb) {
    uint32_t value = 0;
    for (int bit = (int)msb; bit >= (int)lsb; --bit) {
        const unsigned byte_index = (127u - (unsigned)bit) / 8u;
        value = (value << 1u) | ((raw[byte_index] >> (bit & 7)) & 1u);
    }
    return value;
}

static uint32_t decode_scaled_value(uint8_t encoded, const uint32_t units[8], unsigned valid_units) {
    static const uint8_t multiplier_x10[16] = {0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80};
    const unsigned unit_code = encoded & 0x07u;
    const unsigned value_code = (encoded >> 3) & 0x0Fu;
    if ((encoded & 0x80u) || unit_code >= valid_units || multiplier_x10[value_code] == 0)
        return 0;
    return (uint32_t)(((uint64_t)units[unit_code] * multiplier_x10[value_code]) / 10u);
}

static uint32_t decode_data_access_time_ns(uint8_t taac) {
    static const uint32_t units_ns[8] = {1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u};
    return decode_scaled_value(taac, units_ns, 8);
}

static uint32_t decode_transfer_rate_hz(uint8_t transfer_speed) {
    static const uint32_t units_hz[8] = {100000u, 1000000u, 10000000u, 100000000u, 0, 0, 0, 0};
    return decode_scaled_value(transfer_speed, units_hz, 4);
}

static sdcard_result_t read_register(uint8_t command, uint8_t raw[16]) {
    if (raw == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    sdcard_result_t result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;

    result = command_result(spi_send_command(command, 0));
    if (result == SDCARD_OK)
        result = spi_receive_data_block(raw, 16);
    spi_deselect_card();
    return result;
}

static sdcard_result_t lba_to_address(uint32_t lba, uint32_t *address) {
    if (address == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    if (driver_state.card.type & CT_BLOCK) {
        *address = lba;
        return SDCARD_OK;
    }
    if (lba > UINT32_MAX / SDCARD_BLOCK_SIZE)
        return SDCARD_ERROR_OUT_OF_RANGE;
    *address = lba * SDCARD_BLOCK_SIZE;
    return SDCARD_OK;
}

static sdcard_result_t sdcard_read_cid_internal(sdcard_cid_t *cid);
static sdcard_result_t sdcard_read_csd_internal(sdcard_csd_t *csd);
static sdcard_result_t sdcard_read_ocr_internal(sdcard_ocr_t *ocr);
static sdcard_result_t sdcard_read_status_internal(sdcard_status_t *status);
static sdcard_result_t sdcard_read_sd_status_internal(sdcard_sd_status_t *status);
static sdcard_result_t sdcard_get_speed_mode_info_internal(sdcard_switch_status_t *status);
static sdcard_result_t refine_card_error(sdcard_result_t original);

/* -------------------------------------------------------------------------- */
/* Card initialization and register decoding                                 */
/* -------------------------------------------------------------------------- */

static sdcard_result_t sdcard_init_internal(sdcard_init_flags_t flags) {
    uint8_t n, command, type, response[4];
    const uint32_t timeout = 1000;
    uint32_t start;
    if ((flags & ~(SDCARD_INIT_CHECK_MEDIA_BEFORE_IO | SDCARD_INIT_ENABLE_CRC)) != 0) {
        return SDCARD_ERROR_INVALID_ARGUMENT;
    }
    const sdcard_media_state_t previous_media_state = driver_state.media.state;
    if (previous_media_state == SDCARD_MEDIA_READY)
        ++driver_state.media.generation;
    if (driver_state.card.initialized)
        (void)sync_internal();
    if (driver_state.bus.active)
        release_spi();
    clear_card_state();
    driver_state.media.state = SDCARD_MEDIA_UNINITIALIZED;
    /* Keep media probing disabled until the CID has been captured below. */
    driver_state.config.init_flags = flags & SDCARD_INIT_ENABLE_CRC;
    init_spi();
    sleep_ms(10);
    spi_set_initial_baud_rate();
    spi_cs_high();
    for (n = 10; n; --n)
        spi_transfer_byte(0xFF); /* At least 80 clocks with CS high. */

    type = 0;
    if (spi_send_command(CMD0, 0) == 1 && (!(flags & SDCARD_INIT_ENABLE_CRC) || spi_send_command(CMD59, 1) <= 1)) {
        start = clock_millis();
        if (spi_send_command(CMD8, 0x1AA) == 1) {
            for (n = 0; n < 4; ++n)
                response[n] = spi_transfer_byte(0xFF);
            if (response[2] == 0x01 && response[3] == 0xAA) {
                while ((uint32_t)(clock_millis() - start) < timeout && spi_send_command(ACMD41, 1UL << 30)) {
                }
                if ((uint32_t)(clock_millis() - start) < timeout && spi_send_command(CMD58, 0) == 0) {
                    for (n = 0; n < 4; ++n)
                        response[n] = spi_transfer_byte(0xFF);
                    type = (response[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;
                }
            }
        } else {
            if (spi_send_command(ACMD41, 0) <= 1) {
                type = CT_SD1;
                command = ACMD41;
            } else {
                type = CT_MMC;
                command = CMD1;
            }
            while ((uint32_t)(clock_millis() - start) < timeout && spi_send_command(command, 0)) {
            }
            if ((uint32_t)(clock_millis() - start) >= timeout || spi_send_command(CMD16, SDCARD_BLOCK_SIZE) != 0)
                type = 0;
        }
    }

    driver_state.card.type = type;
    spi_deselect_card();
    if (type == 0) {
        clear_card_state();
        driver_state.config.init_flags = SDCARD_INIT_NONE;
        driver_state.media.state = previous_media_state == SDCARD_MEDIA_CHANGED || previous_media_state == SDCARD_MEDIA_AVAILABLE
                                       ? SDCARD_MEDIA_CHANGED
                                       : SDCARD_MEDIA_UNINITIALIZED;
        release_spi();
        return SDCARD_ERROR_NOT_READY;
    }

    spi_set_data_baud_rate();
    driver_state.card.initialized = true;
    driver_state.media.state = SDCARD_MEDIA_READY;

    sdcard_csd_t csd;
    sdcard_result_t result = sdcard_read_csd_internal(&csd);
    sdcard_cid_t cid;
    if (result == SDCARD_OK)
        result = sdcard_read_cid_internal(&cid);
    if (result != SDCARD_OK) {
        clear_card_state();
        driver_state.media.state = previous_media_state == SDCARD_MEDIA_CHANGED || previous_media_state == SDCARD_MEDIA_AVAILABLE
                                       ? SDCARD_MEDIA_CHANGED
                                       : SDCARD_MEDIA_UNINITIALIZED;
        release_spi();
        return result;
    }
    driver_state.card.sector_count = csd.sector_count;
    driver_state.card.erase_sector_size = csd.erase_sector_size;
    driver_state.card.default_max_baud_rate = csd.max_transfer_rate_hz != 0 ? csd.max_transfer_rate_hz : 25000000u;
    driver_state.card.command_classes = csd.command_classes;
    driver_state.card.write_protected = csd.reg.bits.permanent_write_protect || csd.reg.bits.temporary_write_protect;
    if (driver_state.bus.actual_baud_rate > driver_state.card.default_max_baud_rate) {
        set_spi_baud_rate(driver_state.card.default_max_baud_rate);
    }
    memcpy(driver_state.media.initialized_cid, cid.reg.raw, sizeof(driver_state.media.initialized_cid));
    driver_state.media.initialized_cid_valid = true;
    driver_state.config.init_flags = flags;
    if (driver_state.media.generation == 0)
        driver_state.media.generation = 1;
    return SDCARD_OK;
}

static sdcard_result_t sdcard_deinit_internal(void) {
    sdcard_result_t result = driver_state.media.state == SDCARD_MEDIA_READY ? sync_internal() : SDCARD_OK;
    if (driver_state.media.state == SDCARD_MEDIA_READY)
        ++driver_state.media.generation;
    clear_card_state();
    driver_state.media.state = SDCARD_MEDIA_UNINITIALIZED;
    driver_state.config.init_flags = SDCARD_INIT_NONE;
    driver_state.media.initialized_cid_valid = false;
    release_spi();
    return result;
}

static sdcard_result_t sdcard_check_media_internal(sdcard_media_event_t *event) {
    if (event != NULL)
        *event = SDCARD_MEDIA_EVENT_NONE;
    if (driver_state.media.state == SDCARD_MEDIA_READY) {
        return check_initialized_media_internal(event);
    }
    if (driver_state.media.state == SDCARD_MEDIA_UNINITIALIZED || !driver_state.bus.active) {
        return SDCARD_ERROR_NOT_READY;
    }

    const sdcard_media_state_t previous_state = driver_state.media.state;
    spi_set_initial_baud_rate();
    spi_cs_high();
    for (unsigned i = 0; i < 10; ++i)
        spi_transfer_byte(0xFF);
    const uint8_t response = spi_send_command(CMD0, 0);
    spi_deselect_card();
    if (response == 1) {
        driver_state.media.state = SDCARD_MEDIA_AVAILABLE;
        if (event != NULL && previous_state != SDCARD_MEDIA_AVAILABLE) {
            *event = SDCARD_MEDIA_EVENT_AVAILABLE;
        }
    } else {
        driver_state.media.state = SDCARD_MEDIA_CHANGED;
        if (event != NULL && previous_state == SDCARD_MEDIA_AVAILABLE) {
            *event = SDCARD_MEDIA_EVENT_REMOVED;
        }
    }
    return SDCARD_ERROR_MEDIA_CHANGED;
}

sdcard_media_state_t sdcard_get_media_state(void) {
    return driver_state.media.state;
}

uint32_t sdcard_get_media_generation(void) {
    return driver_state.media.generation;
}

bool sdcard_is_ready(void) {
    return ready_state_result() == SDCARD_OK;
}

sdcard_type_t sdcard_get_type(void) {
    if (driver_state.card.type & CT_MMC)
        return SDCARD_TYPE_MMC_V3;
    if (driver_state.card.type & CT_SD1)
        return SDCARD_TYPE_SD_V1;
    if ((driver_state.card.type & (CT_SD2 | CT_BLOCK)) == (CT_SD2 | CT_BLOCK)) {
        return SDCARD_TYPE_SDHC_SDXC;
    }
    if (driver_state.card.type & CT_SD2)
        return SDCARD_TYPE_SD_V2;
    return SDCARD_TYPE_UNKNOWN;
}

/* -------------------------------------------------------------------------- */
/* Blocking data path                                                         */
/* -------------------------------------------------------------------------- */

static sdcard_result_t sdcard_read_blocks_internal(uint32_t lba, uint8_t *buffer, size_t count) {
    uint32_t address;
    if (buffer == NULL || count == 0)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    sdcard_result_t result = ready_state_result();
    if (result != SDCARD_OK)
        return result;
    result = pre_operation_internal();
    if (result == SDCARD_OK)
        result = validate_range(lba, count);
    if (result == SDCARD_OK)
        result = lba_to_address(lba, &address);
    if (result != SDCARD_OK)
        return result;

    if (count == 1) {
        result = command_result(spi_send_command(CMD17, address));
        if (result == SDCARD_OK)
            result = spi_receive_data_block(buffer, SDCARD_BLOCK_SIZE);
    } else {
        result = command_result(spi_send_command(CMD18, address));
        for (size_t remaining = count; result == SDCARD_OK && remaining != 0; --remaining) {
            result = spi_receive_data_block(buffer, SDCARD_BLOCK_SIZE);
            if (result != SDCARD_OK)
                break;
            buffer += SDCARD_BLOCK_SIZE;
        }
        const sdcard_result_t stop_result = command_result(spi_send_command(CMD12, 0));
        if (result == SDCARD_OK)
            result = stop_result;
    }
    spi_deselect_card();
    return refine_card_error(result);
}

static void spi_transmit_bytes(const uint8_t *buffer, size_t length) {
    spi_write_blocking(SDCARD_SPI_BUS, buffer, length);
}

static sdcard_result_t spi_transmit_data_block(const uint8_t *buffer, uint8_t token) {
    if (!spi_wait_card_ready(500))
        return SDCARD_ERROR_TIMEOUT;
    spi_transfer_byte(token);
    if (token != 0xFD) {
        spi_transmit_bytes(buffer, SDCARD_BLOCK_SIZE);
        const uint16_t crc = data_crc_enabled() ? crc16_ccitt(buffer, SDCARD_BLOCK_SIZE) : 0xFFFFu;
        spi_transfer_byte((uint8_t)(crc >> 8));
        spi_transfer_byte((uint8_t)crc);
        const uint8_t response = spi_transfer_byte(0xFF) & 0x1Fu;
        if (response == 0x0Bu)
            return SDCARD_ERROR_CRC;
        if (response != 0x05u)
            return SDCARD_ERROR_IO;
    }
    return SDCARD_OK;
}

static sdcard_result_t card_status_error(const sdcard_status_t *status) {
    if (status->r2.bits.card_locked)
        return SDCARD_ERROR_LOCKED;
    if (status->r2.bits.write_protect_violation || status->r2.bits.wp_erase_skip) {
        return SDCARD_ERROR_WRITE_PROTECTED;
    }
    if (status->r1.bits.command_crc_error)
        return SDCARD_ERROR_CRC;
    if (status->r1.bits.address_error || status->r1.bits.parameter_error || status->r2.bits.out_of_range ||
        status->r2.bits.erase_parameter) {
        return SDCARD_ERROR_OUT_OF_RANGE;
    }
    if (status->r1.bits.illegal_command)
        return SDCARD_ERROR_UNSUPPORTED;
    if (status->r1.raw != 0 || status->r2.raw != 0)
        return SDCARD_ERROR_IO;
    return SDCARD_OK;
}

static sdcard_result_t refine_card_error(sdcard_result_t original) {
    if (original == SDCARD_OK || original == SDCARD_ERROR_TIMEOUT || original == SDCARD_ERROR_CRC)
        return original;
    sdcard_status_t status;
    if (sdcard_read_status_internal(&status) != SDCARD_OK)
        return original;
    const sdcard_result_t status_result = card_status_error(&status);
    return status_result == SDCARD_OK ? original : status_result;
}

static sdcard_result_t sdcard_write_blocks_internal(uint32_t lba, const uint8_t *buffer, size_t count) {
    uint32_t address;
    if (buffer == NULL || count == 0 || count > UINT32_MAX)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    sdcard_result_t result = ready_state_result();
    if (result != SDCARD_OK)
        return result;
    result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;
    if (driver_state.card.write_protected)
        return SDCARD_ERROR_WRITE_PROTECTED;
    result = validate_range(lba, count);
    if (result == SDCARD_OK)
        result = lba_to_address(lba, &address);
    if (result != SDCARD_OK)
        return result;

    if (count == 1) {
        result = command_result(spi_send_command(CMD24, address));
        if (result == SDCARD_OK)
            result = spi_transmit_data_block(buffer, 0xFE);
    } else {
        if (driver_state.card.type & CT_SDC)
            (void)spi_send_command(ACMD23, (uint32_t)count);
        result = command_result(spi_send_command(CMD25, address));
        const bool transfer_started = result == SDCARD_OK;
        for (size_t remaining = count; result == SDCARD_OK && remaining != 0; --remaining) {
            result = spi_transmit_data_block(buffer, 0xFC);
            if (result == SDCARD_OK) {
                buffer += SDCARD_BLOCK_SIZE;
            }
        }
        if (transfer_started) {
            const sdcard_result_t stop_result = spi_transmit_data_block(NULL, 0xFD);
            if (result == SDCARD_OK)
                result = stop_result;
        }
    }
    spi_deselect_card();
    return refine_card_error(result);
}

static sdcard_result_t sdcard_sync_internal(void) {
    return pre_operation_internal();
}

static sdcard_result_t sdcard_read_cid_internal(sdcard_cid_t *cid) {
    if (cid == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    memset(cid, 0, sizeof(*cid));
    sdcard_result_t result = read_register(CMD10, cid->reg.raw);
    if (result != SDCARD_OK)
        return result;

    if (driver_state.card.type & CT_MMC) {
        memcpy(cid->oem_id, cid->reg.mmc.opaque_oem_id, 2);
        memcpy(cid->product_name, cid->reg.mmc.opaque_product_name, 6);
        cid->serial_number =
            ((uint32_t)cid->reg.raw[10] << 24) | ((uint32_t)cid->reg.raw[11] << 16) | ((uint32_t)cid->reg.raw[12] << 8) | cid->reg.raw[13];
        cid->manufacturing_year = 1997u + cid->reg.mmc.manufacturing_year_offset;
        cid->manufacturing_month = cid->reg.mmc.manufacturing_month;
    } else {
        memcpy(cid->oem_id, cid->reg.sd.opaque_oem_id, 2);
        memcpy(cid->product_name, cid->reg.sd.opaque_product_name, 5);
        cid->serial_number =
            ((uint32_t)cid->reg.raw[9] << 24) | ((uint32_t)cid->reg.raw[10] << 16) | ((uint32_t)cid->reg.raw[11] << 8) | cid->reg.raw[12];
        cid->manufacturing_year = 2000u + (((uint16_t)cid->reg.raw[13] & 0x0Fu) << 4) + (cid->reg.raw[14] >> 4);
        cid->manufacturing_month = cid->reg.raw[14] & 0x0F;
    }
    return SDCARD_OK;
}

static sdcard_result_t sdcard_read_csd_internal(sdcard_csd_t *csd) {
    if (csd == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    memset(csd, 0, sizeof(*csd));
    sdcard_result_t result = read_register(CMD9, csd->reg.raw);
    if (result != SDCARD_OK)
        return result;

    const sdcard_csd_bits_t *bits = &csd->reg.bits;
    csd->command_classes = (uint16_t)extract_bits(csd->reg.raw, 95, 84);
    csd->write_block_length = (uint8_t)extract_bits(csd->reg.raw, 25, 22);
    csd->data_access_time_ns = decode_data_access_time_ns(bits->data_access_time);
    csd->data_access_clocks = (uint32_t)bits->data_access_clocks * 100u;
    csd->max_transfer_rate_hz = decode_transfer_rate_hz(bits->transfer_speed);

    const uint32_t erase_write_blocks = bits->erase_single_block_enabled ? 1u : extract_bits(csd->reg.raw, 45, 39) + 1u;
    if (csd->write_block_length < 32) {
        const uint64_t erase_bytes = (uint64_t)erase_write_blocks << csd->write_block_length;
        csd->erase_sector_size = (uint32_t)((erase_bytes + SDCARD_BLOCK_SIZE - 1u) / SDCARD_BLOCK_SIZE);
    }

    if (bits->structure == 1) {
        const uint32_t c_size = extract_bits(csd->reg.raw, 69, 48);
        csd->sector_count = ((uint64_t)c_size + 1u) << 10;
        csd->capacity_bytes = csd->sector_count * SDCARD_BLOCK_SIZE;
    } else if (bits->structure == 0) {
        const uint32_t c_size = extract_bits(csd->reg.raw, 73, 62);
        const uint32_t c_size_mult = extract_bits(csd->reg.raw, 49, 47);
        csd->capacity_bytes = ((uint64_t)c_size + 1u) * (1u << (c_size_mult + 2u)) * (1u << bits->read_block_length);
        csd->sector_count = csd->capacity_bytes / SDCARD_BLOCK_SIZE;
    } else {
        return SDCARD_ERROR_UNSUPPORTED;
    }
    return SDCARD_OK;
}

static sdcard_result_t sdcard_read_ocr_internal(sdcard_ocr_t *ocr) {
    if (ocr == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    memset(ocr, 0, sizeof(*ocr));
    sdcard_result_t result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;

    result = command_result(spi_send_command(CMD58, 0));
    if (result != SDCARD_OK) {
        spi_deselect_card();
        return result;
    }
    uint8_t raw[4];
    for (unsigned i = 0; i < 4; ++i)
        raw[i] = spi_transfer_byte(0xFF);
    spi_deselect_card();
    ocr->raw = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
    return SDCARD_OK;
}

static sdcard_result_t sdcard_read_status_internal(sdcard_status_t *status) {
    if (status == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    memset(status, 0, sizeof(*status));
    sdcard_result_t result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;

    status->r1.raw = spi_send_command(CMD13, 0);
    if (status->r1.raw == 0xFF || (status->r1.raw & 0x80u)) {
        result = SDCARD_ERROR_TIMEOUT;
    } else {
        status->r2.raw = spi_transfer_byte(0xFF);
        result = SDCARD_OK;
    }
    spi_deselect_card();
    return result;
}

static sdcard_result_t sdcard_read_scr_internal(sdcard_scr_t *scr) {
    if (scr == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    sdcard_result_t result = ready_state_result();
    if (result != SDCARD_OK)
        return result;
    result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;
    if (!(driver_state.card.type & CT_SDC))
        return SDCARD_ERROR_UNSUPPORTED;
    memset(scr, 0, sizeof(*scr));

    result = command_result(spi_send_command(ACMD51, 0));
    if (result == SDCARD_OK)
        result = spi_receive_data_block(scr->reg.raw, sizeof(scr->reg.raw));
    spi_deselect_card();
    if (result == SDCARD_OK) {
        scr->specx = (uint8_t)((scr->reg.bits.opaque_specx_high << 2) | scr->reg.bits.opaque_specx_low);
    }
    return result;
}

static uint64_t decode_allocation_unit_size(uint8_t code) {
    static const uint8_t size_mib[16] = {0, 0, 0, 0, 0, 0, 0, 0, 2, 4, 8, 12, 16, 24, 32, 64};
    if (code >= 1 && code <= 7)
        return (uint64_t)16u * 1024u << (code - 1u);
    return (uint64_t)size_mib[code & 0x0Fu] * 1024u * 1024u;
}

static sdcard_result_t sdcard_read_sd_status_internal(sdcard_sd_status_t *status) {
    if (status == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    sdcard_result_t result = ready_state_result();
    if (result != SDCARD_OK)
        return result;
    result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;
    if (!(driver_state.card.type & CT_SDC))
        return SDCARD_ERROR_UNSUPPORTED;
    memset(status, 0, sizeof(*status));

    result = command_result(spi_send_command(ACMD13, 0));
    if (result == SDCARD_OK) {
        (void)spi_transfer_byte(0xFF); /* Second byte of the R2 response. */
        result = spi_receive_data_block(status->reg.raw, sizeof(status->reg.raw));
    }
    spi_deselect_card();
    if (result == SDCARD_OK) {
        status->card_type = ((uint16_t)status->reg.raw[2] << 8) | status->reg.raw[3];
        status->protected_area_size_raw = ((uint32_t)status->reg.raw[4] << 24) | ((uint32_t)status->reg.raw[5] << 16) |
                                          ((uint32_t)status->reg.raw[6] << 8) | status->reg.raw[7];
        status->erase_size = ((uint16_t)status->reg.raw[11] << 8) | status->reg.raw[12];
        status->allocation_unit_size_bytes = decode_allocation_unit_size(status->reg.bits.allocation_unit_size);
    }
    return result;
}

static void decode_switch_status(sdcard_switch_status_t *status) {
    status->max_current_ma = ((uint16_t)status->raw[0] << 8) | status->raw[1];
    status->access_mode_support = ((uint16_t)status->raw[12] << 8) | status->raw[13];
    status->selected_access_mode = status->raw[16] & 0x0Fu;
}

static sdcard_result_t read_switch_status(uint32_t argument, sdcard_switch_status_t *status) {
    sdcard_result_t result = command_result(spi_send_command(CMD6, argument));
    if (result == SDCARD_OK)
        result = spi_receive_data_block(status->raw, sizeof(status->raw));
    spi_deselect_card();
    if (result == SDCARD_OK)
        decode_switch_status(status);
    return result;
}

static sdcard_result_t sdcard_get_speed_mode_info_internal(sdcard_switch_status_t *status) {
    if (status == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    sdcard_result_t result = ready_state_result();
    if (result != SDCARD_OK)
        return result;
    result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;
    if (!(driver_state.card.type & CT_SDC) || !(driver_state.card.command_classes & (1u << 10))) {
        return SDCARD_ERROR_UNSUPPORTED;
    }
    memset(status, 0, sizeof(*status));
    return read_switch_status(0x00FFFFFFu, status);
}

static sdcard_result_t sdcard_set_speed_mode_internal(sdcard_speed_mode_t mode, sdcard_switch_status_t *status) {
    if (mode != SDCARD_SPEED_DEFAULT && mode != SDCARD_SPEED_HIGH) {
        return SDCARD_ERROR_INVALID_ARGUMENT;
    }
    sdcard_result_t result = ready_state_result();
    if (result != SDCARD_OK)
        return result;

    sdcard_switch_status_t local_status;
    result = sdcard_get_speed_mode_info_internal(&local_status);
    if (result != SDCARD_OK)
        return result;
    if (!(local_status.access_mode_support & (1u << mode)))
        return SDCARD_ERROR_UNSUPPORTED;
    result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;
    if (mode == SDCARD_SPEED_DEFAULT && driver_state.bus.actual_baud_rate > driver_state.card.default_max_baud_rate) {
        set_spi_baud_rate(driver_state.card.default_max_baud_rate);
    }
    memset(&local_status, 0, sizeof(local_status));
    result = read_switch_status(0x80FFFFF0u | (uint32_t)mode, &local_status);
    if (status != NULL)
        *status = local_status;
    if (result == SDCARD_OK && local_status.selected_access_mode != (uint8_t)mode) {
        return SDCARD_ERROR_COMMAND_REJECTED;
    }
    if (result == SDCARD_OK)
        driver_state.card.speed_mode = mode;
    return result;
}

static sdcard_result_t sdcard_set_bus_baud_rate_internal(uint32_t requested_hz, uint32_t *actual_hz) {
    if (requested_hz == 0)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    sdcard_result_t result = ready_state_result();
    if (result != SDCARD_OK)
        return result;
    result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;
    const uint32_t limit = driver_state.card.speed_mode == SDCARD_SPEED_HIGH ? 50000000u : driver_state.card.default_max_baud_rate;
    if (requested_hz > limit)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    const uint32_t actual = set_spi_baud_rate(requested_hz);
    if (actual_hz != NULL)
        *actual_hz = actual;
    return SDCARD_OK;
}

uint32_t sdcard_get_bus_baud_rate(void) {
    return driver_state.bus.active ? driver_state.bus.actual_baud_rate : 0;
}

static sdcard_result_t sdcard_read_info_internal(sdcard_info_t *info) {
    if (info == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));
    info->type = sdcard_get_type();
    sdcard_result_t result = sdcard_read_cid_internal(&info->cid);
    if (result == SDCARD_OK)
        result = sdcard_read_csd_internal(&info->csd);
    if (result == SDCARD_OK)
        result = sdcard_read_ocr_internal(&info->ocr);
    return result;
}

static sdcard_result_t sdcard_get_erase_block_size_internal(uint32_t *sector_count) {
    if (sector_count == NULL)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    sdcard_result_t state_result = ready_state_result();
    if (state_result != SDCARD_OK)
        return state_result;
    state_result = pre_operation_internal();
    if (state_result != SDCARD_OK)
        return state_result;

    if (driver_state.card.type & CT_SD2) {
        sdcard_sd_status_t status;
        sdcard_result_t result = sdcard_read_sd_status_internal(&status);
        if (result != SDCARD_OK)
            return result;
        if (status.allocation_unit_size_bytes != 0) {
            const uint64_t sectors = status.allocation_unit_size_bytes / SDCARD_BLOCK_SIZE;
            if (sectors > UINT32_MAX)
                return SDCARD_ERROR_UNSUPPORTED;
            *sector_count = (uint32_t)sectors;
            return SDCARD_OK;
        }
    }
    if (driver_state.card.erase_sector_size == 0)
        return SDCARD_ERROR_UNSUPPORTED;
    *sector_count = driver_state.card.erase_sector_size;
    return SDCARD_OK;
}

static sdcard_result_t sdcard_erase_blocks_internal(uint32_t first_lba, uint32_t last_lba) {
    sdcard_result_t result = ready_state_result();
    if (result != SDCARD_OK)
        return result;
    result = pre_operation_internal();
    if (result != SDCARD_OK)
        return result;
    if (!(driver_state.card.type & CT_SDC))
        return SDCARD_ERROR_UNSUPPORTED;
    if (first_lba > last_lba)
        return SDCARD_ERROR_INVALID_ARGUMENT;
    if (driver_state.card.write_protected)
        return SDCARD_ERROR_WRITE_PROTECTED;
    if (driver_state.card.sector_count == 0 || (uint64_t)last_lba >= driver_state.card.sector_count) {
        return SDCARD_ERROR_OUT_OF_RANGE;
    }
    if (driver_state.card.erase_sector_size > 1 &&
        (first_lba % driver_state.card.erase_sector_size != 0 || ((uint64_t)last_lba + 1u) % driver_state.card.erase_sector_size != 0)) {
        return SDCARD_ERROR_INVALID_ARGUMENT;
    }
    uint32_t first_address, last_address;
    result = lba_to_address(first_lba, &first_address);
    if (result == SDCARD_OK)
        result = lba_to_address(last_lba, &last_address);
    if (result != SDCARD_OK)
        return result;

    result = command_result(spi_send_command(CMD32, first_address));
    if (result == SDCARD_OK)
        result = command_result(spi_send_command(CMD33, last_address));
    if (result == SDCARD_OK)
        result = command_result(spi_send_command(CMD38, 0));
    if (result == SDCARD_OK && !spi_wait_card_ready(30000))
        result = SDCARD_ERROR_TIMEOUT;
    spi_deselect_card();
    return refine_card_error(result);
}

sdcard_result_t sdcard_init(sdcard_init_flags_t flags) {
    arbitration_init_once();
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_init_internal(flags);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_deinit(void) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_deinit_internal();
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_check_media(sdcard_media_event_t *event) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_check_media_internal(event);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_read_blocks(uint32_t lba, uint8_t *buffer, size_t count) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = blocking_dma_transport_enabled() ? blocking_dma_transfer(ASYNC_OPERATION_READ, lba, buffer, NULL, count)
                                              : sdcard_read_blocks_internal(lba, buffer, count);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_write_blocks(uint32_t lba, const uint8_t *buffer, size_t count) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = blocking_dma_transport_enabled() ? blocking_dma_transfer(ASYNC_OPERATION_WRITE, lba, NULL, buffer, count)
                                              : sdcard_write_blocks_internal(lba, buffer, count);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_sync(void) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_sync_internal();
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_read_cid(sdcard_cid_t *cid) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_read_cid_internal(cid);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_read_csd(sdcard_csd_t *csd) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_read_csd_internal(csd);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_read_ocr(sdcard_ocr_t *ocr) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_read_ocr_internal(ocr);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_read_status(sdcard_status_t *status) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_read_status_internal(status);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_read_scr(sdcard_scr_t *scr) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_read_scr_internal(scr);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_read_sd_status(sdcard_sd_status_t *status) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_read_sd_status_internal(status);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_get_speed_mode_info(sdcard_switch_status_t *status) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_get_speed_mode_info_internal(status);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_set_speed_mode(sdcard_speed_mode_t mode, sdcard_switch_status_t *status) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_set_speed_mode_internal(mode, status);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_set_bus_baud_rate(uint32_t requested_hz, uint32_t *actual_hz) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_set_bus_baud_rate_internal(requested_hz, actual_hz);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_read_info(sdcard_info_t *info) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_read_info_internal(info);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_get_erase_block_size(uint32_t *sector_count) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_get_erase_block_size_internal(sector_count);
    blocking_exit();
    return result;
}

sdcard_result_t sdcard_erase_blocks(uint32_t first_lba, uint32_t last_lba) {
    sdcard_result_t result = blocking_enter();
    if (result != SDCARD_OK)
        return result;
    result = sdcard_erase_blocks_internal(first_lba, last_lba);
    blocking_exit();
    return result;
}

/* -------------------------------------------------------------------------- */
/* Async DMA transport                                                       */
/* -------------------------------------------------------------------------- */

static void async_drain_spi_rx_fifo(void) {
    while (spi_is_readable(SDCARD_SPI_BUS)) {
        (void)spi_get_hw(SDCARD_SPI_BUS)->dr;
    }
}

static void async_dma_irq_acknowledge(void) {
    const uint irq_index = driver_state.async.config.dma_irq_index;
    dma_irqn_acknowledge_channel(irq_index, (uint)driver_state.async.config.tx_dma_channel);
    dma_irqn_acknowledge_channel(irq_index, (uint)driver_state.async.config.rx_dma_channel);
}

static void async_dma_irq_arm(void) {
    if (!driver_state.async.config.use_dma_irq)
        return;
    const uint irq_index = driver_state.async.config.dma_irq_index;
    const uint32_t channel_mask = async_dma_channel_mask();
    dma_irqn_set_channel_mask_enabled(irq_index, channel_mask, false);
    async_dma_irq_acknowledge();
    __atomic_store_n(&driver_state.async.dma_irq_events, 0u, __ATOMIC_RELEASE);
#if SDCARD_ASYNC_ENABLE_STATS
    __atomic_store_n(&driver_state.async.dma_irq_event_time_us, 0u, __ATOMIC_RELEASE);
#endif
    dma_irqn_set_channel_mask_enabled(irq_index, channel_mask, true);
}

static void async_dma_irq_disarm(void) {
    if (!driver_state.async.config.use_dma_irq)
        return;
    const uint irq_index = driver_state.async.config.dma_irq_index;
    dma_irqn_set_channel_mask_enabled(irq_index, async_dma_channel_mask(), false);
    async_dma_irq_acknowledge();
    __atomic_store_n(&driver_state.async.dma_irq_events, 0u, __ATOMIC_RELEASE);
#if SDCARD_ASYNC_ENABLE_STATS
    __atomic_store_n(&driver_state.async.dma_irq_event_time_us, 0u, __ATOMIC_RELEASE);
#endif
}

static void async_start_read_dma(uint8_t *destination) {
    async_drain_spi_rx_fifo();
    async_dma_irq_arm();
    dma_channel_config rx_config = dma_channel_get_default_config((uint)driver_state.async.config.rx_dma_channel);
    channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_8);
    channel_config_set_read_increment(&rx_config, false);
    channel_config_set_write_increment(&rx_config, true);
    channel_config_set_dreq(&rx_config, spi_get_dreq(SDCARD_SPI_BUS, false));

    dma_channel_config tx_config = dma_channel_get_default_config((uint)driver_state.async.config.tx_dma_channel);
    channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_8);
    channel_config_set_read_increment(&tx_config, false);
    channel_config_set_write_increment(&tx_config, false);
    channel_config_set_dreq(&tx_config, spi_get_dreq(SDCARD_SPI_BUS, true));

    driver_state.async.tx_fill = 0xFF;
    dma_channel_configure((uint)driver_state.async.config.rx_dma_channel, &rx_config, destination, &spi_get_hw(SDCARD_SPI_BUS)->dr,
                          SDCARD_BLOCK_SIZE, false);
    dma_channel_configure((uint)driver_state.async.config.tx_dma_channel, &tx_config, &spi_get_hw(SDCARD_SPI_BUS)->dr,
                          &driver_state.async.tx_fill, SDCARD_BLOCK_SIZE, false);
    driver_state.async.request.dma_active = true;
    dma_start_channel_mask((1u << (uint)driver_state.async.config.rx_dma_channel) | (1u << (uint)driver_state.async.config.tx_dma_channel));
}

static void async_start_write_dma(const uint8_t *source) {
    async_drain_spi_rx_fifo();
    async_dma_irq_arm();
    dma_channel_config rx_config = dma_channel_get_default_config((uint)driver_state.async.config.rx_dma_channel);
    channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_8);
    channel_config_set_read_increment(&rx_config, false);
    channel_config_set_write_increment(&rx_config, false);
    channel_config_set_dreq(&rx_config, spi_get_dreq(SDCARD_SPI_BUS, false));

    dma_channel_config tx_config = dma_channel_get_default_config((uint)driver_state.async.config.tx_dma_channel);
    channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_8);
    channel_config_set_read_increment(&tx_config, true);
    channel_config_set_write_increment(&tx_config, false);
    channel_config_set_dreq(&tx_config, spi_get_dreq(SDCARD_SPI_BUS, true));

    dma_channel_configure((uint)driver_state.async.config.rx_dma_channel, &rx_config, &driver_state.async.rx_sink,
                          &spi_get_hw(SDCARD_SPI_BUS)->dr, SDCARD_BLOCK_SIZE, false);
    dma_channel_configure((uint)driver_state.async.config.tx_dma_channel, &tx_config, &spi_get_hw(SDCARD_SPI_BUS)->dr, source,
                          SDCARD_BLOCK_SIZE, false);
    driver_state.async.request.dma_active = true;
    dma_start_channel_mask((1u << (uint)driver_state.async.config.rx_dma_channel) | (1u << (uint)driver_state.async.config.tx_dma_channel));
}

typedef enum {
    ASYNC_DMA_WAITING,
    ASYNC_DMA_COMPLETE,
    ASYNC_DMA_ERROR,
} async_dma_payload_state_t;

static uint32_t async_dma_error_flags(uint channel) {
    return dma_hw->ch[channel].ctrl_trig & (DMA_CH0_CTRL_TRIG_READ_ERROR_BITS | DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS);
}

static async_dma_payload_state_t async_dma_payload_state(void) {
    const uint32_t tx_error = async_dma_error_flags((uint)driver_state.async.config.tx_dma_channel);
    const uint32_t rx_error = async_dma_error_flags((uint)driver_state.async.config.rx_dma_channel);
    if (tx_error != 0 || rx_error != 0) {
        if (tx_error != 0) {
            hw_set_bits(&dma_hw->ch[(uint)driver_state.async.config.tx_dma_channel].ctrl_trig, tx_error);
        }
        if (rx_error != 0) {
            hw_set_bits(&dma_hw->ch[(uint)driver_state.async.config.rx_dma_channel].ctrl_trig, rx_error);
        }
#if SDCARD_ASYNC_ENABLE_STATS
        critical_section_enter_blocking(&driver_state.arbitration.lock);
        ++driver_state.async.dma_errors;
        critical_section_exit(&driver_state.arbitration.lock);
#endif
        return ASYNC_DMA_ERROR;
    }

    if (driver_state.async.config.use_dma_irq) {
        const uint32_t channel_mask = async_dma_channel_mask();
        const uint32_t events = __atomic_load_n(&driver_state.async.dma_irq_events, __ATOMIC_ACQUIRE);
        if ((events & channel_mask) != channel_mask)
            return ASYNC_DMA_WAITING;
        if (dma_channel_is_busy((uint)driver_state.async.config.rx_dma_channel) ||
            dma_channel_is_busy((uint)driver_state.async.config.tx_dma_channel)) {
            return ASYNC_DMA_WAITING;
        }
#if SDCARD_ASYNC_ENABLE_STATS
        const uint32_t event_time_us = __atomic_load_n(&driver_state.async.dma_irq_event_time_us, __ATOMIC_ACQUIRE);
        if (event_time_us != 0) {
            const uint32_t latency_us = time_us_32() - event_time_us;
            critical_section_enter_blocking(&driver_state.arbitration.lock);
            ++driver_state.async.dma_irq_payloads;
            driver_state.async.dma_irq_latency_us_total += latency_us;
            if (latency_us > driver_state.async.dma_irq_latency_us_max) {
                driver_state.async.dma_irq_latency_us_max = latency_us;
            }
            critical_section_exit(&driver_state.arbitration.lock);
        }
#endif
        __atomic_fetch_and(&driver_state.async.dma_irq_events, ~channel_mask, __ATOMIC_ACQ_REL);
#if SDCARD_ASYNC_ENABLE_STATS
        __atomic_store_n(&driver_state.async.dma_irq_event_time_us, 0u, __ATOMIC_RELEASE);
#endif
        return ASYNC_DMA_COMPLETE;
    }
    return !dma_channel_is_busy((uint)driver_state.async.config.rx_dma_channel) &&
                   !dma_channel_is_busy((uint)driver_state.async.config.tx_dma_channel)
               ? ASYNC_DMA_COMPLETE
               : ASYNC_DMA_WAITING;
}

static void async_cleanup_dma(void) {
    async_dma_irq_disarm();
    dma_channel_cleanup((uint)driver_state.async.config.tx_dma_channel);
    dma_channel_cleanup((uint)driver_state.async.config.rx_dma_channel);
    driver_state.async.request.dma_active = false;
    while (spi_is_busy(SDCARD_SPI_BUS))
        tight_loop_contents();
    async_drain_spi_rx_fifo();
}

static void async_set_phase(sdcard_async_phase_t phase) {
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    driver_state.async.phase = phase;
    critical_section_exit(&driver_state.arbitration.lock);
}

static sdcard_async_phase_t async_get_phase(void) {
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    const sdcard_async_phase_t phase = driver_state.async.phase;
    critical_section_exit(&driver_state.arbitration.lock);
    return phase;
}

static bool async_cancel_was_requested(void) {
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    const bool requested = driver_state.async.cancel_requested;
    critical_section_exit(&driver_state.arbitration.lock);
    return requested;
}

static bool async_request_timed_out(void) {
    return driver_state.async.request.deadline_enabled && (int64_t)(time_us_64() - driver_state.async.request.deadline_us) >= 0;
}

static uint8_t async_send_command_selected(uint8_t command, uint32_t argument) {
    (void)spi_transfer_byte((uint8_t)(0x40u | command));
    (void)spi_transfer_byte((uint8_t)(argument >> 24));
    (void)spi_transfer_byte((uint8_t)(argument >> 16));
    (void)spi_transfer_byte((uint8_t)(argument >> 8));
    (void)spi_transfer_byte((uint8_t)argument);
    (void)spi_transfer_byte(data_crc_enabled() ? crc7_command(command, argument) : 0x01);
    if (command == CMD12)
        (void)spi_transfer_byte(0xFF);

    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        const uint8_t response = spi_transfer_byte(0xFF);
        if ((response & 0x80u) == 0)
            return response;
    }
    return 0xFF;
}

static bool async_spi_wait_ready_bounded(void) {
    for (unsigned poll = 0; poll < SDCARD_ASYNC_TASK_SPI_POLL_BUDGET; ++poll) {
        if (spi_transfer_byte(0xFF) == 0xFF)
            return true;
    }
    return false;
}

static uint8_t async_spi_poll_token_bounded(void) {
    uint8_t token = 0xFF;
    for (unsigned poll = 0; poll < SDCARD_ASYNC_TASK_SPI_POLL_BUDGET; ++poll) {
        token = spi_transfer_byte(0xFF);
        if (token != 0xFF)
            return token;
    }
    return token;
}

static void async_begin_selection(sdcard_async_phase_t wait_phase) {
    spi_deselect_card();
    spi_cs_low();
    (void)spi_transfer_byte(0xFF);
    async_set_phase(wait_phase);
}

static bool async_progress_threshold_reached(void) {
    const size_t interval = driver_state.async.request.progress_interval_blocks;
    return interval != 0 && driver_state.async.request.transferred % interval == 0;
}

static bool async_queue_progress(sdcard_async_phase_t resume_phase) {
    if (!async_progress_threshold_reached() || driver_state.async.request.transferred >= driver_state.async.request.count) {
        return false;
    }
    driver_state.async.progress_resume_phase = resume_phase;
    async_set_phase(ASYNC_PHASE_PROGRESS_PENDING);
    return true;
}

static void async_complete_finalize(sdcard_result_t result) {
    const bool transfer_failed = result != SDCARD_OK && driver_state.async.request.transfer_started;
    async_cleanup_dma();
    if (transfer_failed && driver_state.async.request.operation == ASYNC_OPERATION_READ && driver_state.async.request.multi_block) {
        (void)async_send_command_selected(CMD12, 0);
    }
    spi_deselect_card();
    if (transfer_failed && driver_state.media.state == SDCARD_MEDIA_READY) {
        invalidate_media(SDCARD_MEDIA_EVENT_REMOVED);
    }
    driver_state.async.request.result = result;
    driver_state.async.pending_callback_events = SDCARD_ASYNC_EVENT_COMPLETE;
    if (result == SDCARD_OK && async_progress_threshold_reached()) {
        driver_state.async.pending_callback_events |= SDCARD_ASYNC_EVENT_PROGRESS;
    }
    async_set_phase(ASYNC_PHASE_COMPLETION_PENDING);
}

static void async_complete(sdcard_result_t result) {
    /* DMA abort and multi-block recovery may spin, so defer every failure from
     * IRQ context. A successful payload is finalizable only after both DMA and
     * SPI report idle; the normal task path remains the recovery authority. */
    if (driver_state.async.task_from_irq_active &&
        (result != SDCARD_OK || driver_state.async.request.dma_active || spi_is_busy(SDCARD_SPI_BUS))) {
        driver_state.async.irq_deferred_result = result;
        driver_state.async.request.result = result;
        async_set_phase(ASYNC_PHASE_IRQ_COMPLETION_PENDING);
        return;
    }
    async_complete_finalize(result);
}

static void async_probe_failed(void) {
    if (++driver_state.async.probe_attempts < 2) {
        async_begin_selection(ASYNC_PHASE_PRECHECK_STATUS_WAIT_READY);
        return;
    }
    if (!driver_state.async.have_successful_probe) {
        invalidate_media(SDCARD_MEDIA_EVENT_REMOVED);
        async_complete(SDCARD_ERROR_MEDIA_CHANGED);
    } else {
        async_complete(SDCARD_ERROR_IO);
    }
}

static void async_process_probe_cid(void) {
    driver_state.async.have_successful_probe = true;
    if (driver_state.media.initialized_cid_valid &&
        memcmp(driver_state.async.probe_candidate, driver_state.media.initialized_cid, 16) == 0) {
        async_begin_selection(ASYNC_PHASE_COMMAND_WAIT_READY);
        return;
    }
    if (driver_state.async.have_mismatch && memcmp(driver_state.async.probe_candidate, driver_state.async.previous_mismatch, 16) == 0) {
        invalidate_media(SDCARD_MEDIA_EVENT_CHANGED);
        async_complete(SDCARD_ERROR_MEDIA_CHANGED);
        return;
    }
    memcpy(driver_state.async.previous_mismatch, driver_state.async.probe_candidate, 16);
    driver_state.async.have_mismatch = true;
    async_probe_failed();
}

static uint32_t async_request_address(void) {
    return (driver_state.card.type & CT_BLOCK) ? driver_state.async.request.lba : driver_state.async.request.lba * SDCARD_BLOCK_SIZE;
}

/* -------------------------------------------------------------------------- */
/* Async request lifecycle                                                    */
/* -------------------------------------------------------------------------- */

static sdcard_result_t async_submit_owned(sdcard_async_operation_t operation, uint32_t lba, uint8_t *read_buffer,
                                          const uint8_t *write_buffer, size_t count, uint32_t timeout_ms, size_t progress_interval_blocks,
                                          sdcard_async_callback_t callback, void *user_data, sdcard_owner_t target_owner,
                                          bool blocking_facade) {
    if ((operation == ASYNC_OPERATION_READ && read_buffer == NULL) || (operation == ASYNC_OPERATION_WRITE && write_buffer == NULL) ||
        callback == NULL || count == 0 || count > UINT32_MAX) {
        return SDCARD_ERROR_INVALID_ARGUMENT;
    }
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    if (!driver_state.async.initialized) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_NOT_READY;
    }
    const bool owner_available = target_owner == SDCARD_OWNER_ASYNC
                                     ? driver_state.arbitration.owner == SDCARD_OWNER_IDLE
                                     : target_owner == SDCARD_OWNER_BLOCKING && driver_state.arbitration.owner == SDCARD_OWNER_BLOCKING;
    if (!owner_available || driver_state.async.phase != ASYNC_PHASE_IDLE) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_BUSY;
    }
    const sdcard_result_t ready = ready_state_result();
    if (ready != SDCARD_OK) {
        critical_section_exit(&driver_state.arbitration.lock);
        return ready;
    }
    if (driver_state.card.sector_count == 0 || (uint64_t)lba >= driver_state.card.sector_count ||
        (uint64_t)count > driver_state.card.sector_count - lba ||
        (!(driver_state.card.type & CT_BLOCK) && lba > UINT32_MAX / SDCARD_BLOCK_SIZE)) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_OUT_OF_RANGE;
    }
    if (operation == ASYNC_OPERATION_WRITE && driver_state.card.write_protected) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_WRITE_PROTECTED;
    }

    driver_state.async.request = (sdcard_async_request_t){
        .deadline_us = timeout_ms == SDCARD_ASYNC_TIMEOUT_INFINITE ? 0 : time_us_64() + (uint64_t)timeout_ms * 1000u,
        .lba = lba,
        .count = count,
        .read_buffer = read_buffer,
        .write_buffer = write_buffer,
        .callback = callback,
        .user_data = user_data,
        .progress_interval_blocks = progress_interval_blocks,
        .read_dma_block_index = 0,
        .operation = operation,
        .deadline_enabled = timeout_ms != SDCARD_ASYNC_TIMEOUT_INFINITE,
        .multi_block = count > 1,
        .blocking_facade = blocking_facade,
    };
    driver_state.async.cancel_requested = false;
    driver_state.arbitration.owner = target_owner;
    driver_state.async.phase = ASYNC_PHASE_QUEUED;
    critical_section_exit(&driver_state.arbitration.lock);
    __sev();
    return SDCARD_OK;
}

static sdcard_result_t async_submit(sdcard_async_operation_t operation, uint32_t lba, uint8_t *read_buffer, const uint8_t *write_buffer,
                                    size_t count, uint32_t timeout_ms, size_t progress_interval_blocks, sdcard_async_callback_t callback,
                                    void *user_data) {
    return async_submit_owned(operation, lba, read_buffer, write_buffer, count, timeout_ms, progress_interval_blocks, callback, user_data,
                              SDCARD_OWNER_ASYNC, false);
}

typedef struct {
    volatile bool done;
    sdcard_result_t result;
    size_t transferred;
} sdcard_blocking_dma_wait_t;

static void blocking_dma_callback(sdcard_async_event_flags_t events, sdcard_result_t result, size_t blocks_transferred, void *user_data) {
    if ((events & SDCARD_ASYNC_EVENT_COMPLETE) == 0)
        return;
    sdcard_blocking_dma_wait_t *wait = user_data;
    wait->result = result;
    wait->transferred = blocks_transferred;
    __atomic_store_n(&wait->done, true, __ATOMIC_RELEASE);
    __sev();
}

static bool blocking_dma_transport_enabled(void) {
    if (driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED)
        return false;
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    const bool enabled = driver_state.async.initialized && driver_state.async.phase == ASYNC_PHASE_IDLE &&
                         driver_state.arbitration.owner == SDCARD_OWNER_BLOCKING &&
                         driver_state.async.config.blocking_transport == SDCARD_BLOCKING_TRANSPORT_DMA &&
                         get_core_num() == driver_state.async.owner_core;
    critical_section_exit(&driver_state.arbitration.lock);
    return enabled;
}

static sdcard_result_t blocking_dma_transfer(sdcard_async_operation_t operation, uint32_t lba, uint8_t *read_buffer,
                                             const uint8_t *write_buffer, size_t count) {
    sdcard_blocking_dma_wait_t wait = {
        .result = SDCARD_ERROR_IO,
    };
    uint32_t timeout_ms;
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    timeout_ms = driver_state.async.config.blocking_timeout_ms;
    critical_section_exit(&driver_state.arbitration.lock);

    sdcard_result_t result = async_submit_owned(operation, lba, read_buffer, write_buffer, count, timeout_ms, 0, blocking_dma_callback,
                                                &wait, SDCARD_OWNER_BLOCKING, true);
    if (result != SDCARD_OK)
        return result;

    while (!__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE)) {
        const sdcard_async_task_hint_t hint = sdcard_async_task();
        if (hint == SDCARD_ASYNC_TASK_WAITING)
            tight_loop_contents();
    }
    return wait.result;
}

/* -------------------------------------------------------------------------- */
/* Public async lifecycle and submission API                                 */
/* -------------------------------------------------------------------------- */

sdcard_async_config_t sdcard_async_get_default_config(void) {
    return (sdcard_async_config_t){
        .tx_dma_channel = -1,
        .rx_dma_channel = -1,
        .dma_irq_index = 0,
        .blocking_timeout_ms = 0,
        .blocking_transport = SDCARD_BLOCKING_TRANSPORT_CPU,
        .use_dma_irq = false,
        .allow_task_from_irq = false,
        .allow_callback_from_irq = false,
    };
}

sdcard_result_t sdcard_async_init(const sdcard_async_config_t *config) {
    if (config == NULL || config->tx_dma_channel < 0 || config->rx_dma_channel < 0 || config->tx_dma_channel >= NUM_DMA_CHANNELS ||
        config->rx_dma_channel >= NUM_DMA_CHANNELS || config->tx_dma_channel == config->rx_dma_channel ||
        config->dma_irq_index >= NUM_DMA_IRQS || config->blocking_transport > SDCARD_BLOCKING_TRANSPORT_DMA ||
        !dma_channel_is_claimed((uint)config->tx_dma_channel) || !dma_channel_is_claimed((uint)config->rx_dma_channel)) {
        return SDCARD_ERROR_INVALID_ARGUMENT;
    }
    if (driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED) {
        return SDCARD_ERROR_NOT_READY;
    }
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    if (driver_state.arbitration.owner != SDCARD_OWNER_IDLE || driver_state.async.initialized) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_BUSY;
    }

    memset(&driver_state.async, 0, sizeof(driver_state.async));
    driver_state.async.config = *config;
    if (driver_state.async.config.blocking_timeout_ms == 0) {
        driver_state.async.config.blocking_timeout_ms = SDCARD_BLOCKING_TIMEOUT_DEFAULT_MS;
    }
    driver_state.async.owner_core = get_core_num();
    driver_state.async.tx_fill = 0xFF;
    if (config->use_dma_irq) {
        const uint irq_num = dma_get_irq_num(config->dma_irq_index);
        const uint32_t channel_mask = async_dma_channel_mask();
        dma_irqn_set_channel_mask_enabled(config->dma_irq_index, channel_mask, false);
        async_dma_irq_acknowledge();
        irq_add_shared_handler(irq_num, async_dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(irq_num, true);
        driver_state.async.dma_irq_handler_installed = true;
    }
    driver_state.async.initialized = true;
    critical_section_exit(&driver_state.arbitration.lock);
    return SDCARD_OK;
}

sdcard_result_t sdcard_async_deinit(void) {
    if (driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED) {
        return SDCARD_ERROR_NOT_READY;
    }
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    if (!driver_state.async.initialized) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_NOT_READY;
    }
    if (driver_state.arbitration.owner != SDCARD_OWNER_IDLE || driver_state.async.phase != ASYNC_PHASE_IDLE) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_BUSY;
    }
    driver_state.async.initialized = false;
    const int tx_dma_channel = driver_state.async.config.tx_dma_channel;
    const int rx_dma_channel = driver_state.async.config.rx_dma_channel;
    const unsigned int dma_irq_index = driver_state.async.config.dma_irq_index;
    const bool dma_irq_enabled = driver_state.async.config.use_dma_irq && driver_state.async.dma_irq_handler_installed;
    critical_section_exit(&driver_state.arbitration.lock);
    if (dma_irq_enabled) {
        const uint irq_num = dma_get_irq_num(dma_irq_index);
        irq_remove_handler(irq_num, async_dma_irq_handler);
        dma_irqn_set_channel_mask_enabled(dma_irq_index, (1u << (uint)tx_dma_channel) | (1u << (uint)rx_dma_channel), false);
        dma_irqn_acknowledge_channel(dma_irq_index, (uint)tx_dma_channel);
        dma_irqn_acknowledge_channel(dma_irq_index, (uint)rx_dma_channel);
    }
    dma_channel_cleanup((uint)tx_dma_channel);
    dma_channel_cleanup((uint)rx_dma_channel);
    return SDCARD_OK;
}

sdcard_result_t sdcard_async_get_stats(sdcard_async_stats_t *stats) {
    if (stats == NULL || driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED) {
        return stats == NULL ? SDCARD_ERROR_INVALID_ARGUMENT : SDCARD_ERROR_NOT_READY;
    }
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    if (!driver_state.async.initialized) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_NOT_READY;
    }
    memset(stats, 0, sizeof(*stats));
#if SDCARD_ASYNC_ENABLE_STATS
    stats->dma_irq_payloads = driver_state.async.dma_irq_payloads;
    stats->dma_irq_latency_us_total = driver_state.async.dma_irq_latency_us_total;
    stats->dma_irq_latency_us_max = driver_state.async.dma_irq_latency_us_max;
    stats->dma_errors = driver_state.async.dma_errors;
#endif
    critical_section_exit(&driver_state.arbitration.lock);
    return SDCARD_OK;
}

sdcard_result_t sdcard_async_reset_stats(void) {
    if (driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED) {
        return SDCARD_ERROR_NOT_READY;
    }
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    if (!driver_state.async.initialized) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_NOT_READY;
    }
#if SDCARD_ASYNC_ENABLE_STATS
    driver_state.async.dma_irq_payloads = 0;
    driver_state.async.dma_irq_latency_us_total = 0;
    driver_state.async.dma_irq_latency_us_max = 0;
    driver_state.async.dma_errors = 0;
#endif
    critical_section_exit(&driver_state.arbitration.lock);
    return SDCARD_OK;
}

sdcard_result_t sdcard_async_get_schedule(sdcard_async_schedule_t *schedule) {
    if (schedule == NULL || driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED) {
        return schedule == NULL ? SDCARD_ERROR_INVALID_ARGUMENT : SDCARD_ERROR_NOT_READY;
    }
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    if (!driver_state.async.initialized) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_NOT_READY;
    }
    *schedule = (sdcard_async_schedule_t){0};
    const sdcard_async_phase_t phase = driver_state.async.phase;
    if (driver_state.arbitration.owner == SDCARD_OWNER_ASYNC && phase != ASYNC_PHASE_IDLE) {
        schedule->request_active = true;
        schedule->task_running = (__atomic_load_n(&driver_state.async.task_gate, __ATOMIC_ACQUIRE) & ASYNC_TASK_GATE_RUNNING) != 0;
        if (phase != ASYNC_PHASE_COMPLETION_PENDING && driver_state.async.request.deadline_enabled) {
            schedule->deadline_valid = true;
            schedule->deadline_us = driver_state.async.request.deadline_us;
        }
        schedule->wake_on_dma_irq =
            driver_state.async.config.use_dma_irq && (phase == ASYNC_PHASE_READ_DMA || phase == ASYNC_PHASE_WRITE_DMA);
        schedule->needs_poll = phase != ASYNC_PHASE_COMPLETION_PENDING && !schedule->wake_on_dma_irq;
    }
    critical_section_exit(&driver_state.arbitration.lock);
    return SDCARD_OK;
}

sdcard_result_t sdcard_read_blocks_async(uint32_t lba, uint8_t *buffer, size_t count, uint32_t timeout_ms, sdcard_async_callback_t callback,
                                         void *user_data) {
    return sdcard_read_blocks_async_ex(lba, buffer, count, timeout_ms, 0, callback, user_data);
}

sdcard_result_t sdcard_read_blocks_async_ex(uint32_t lba, uint8_t *buffer, size_t count, uint32_t timeout_ms,
                                            size_t progress_interval_blocks, sdcard_async_callback_t callback, void *user_data) {
    return async_submit(ASYNC_OPERATION_READ, lba, buffer, NULL, count, timeout_ms, progress_interval_blocks, callback, user_data);
}

sdcard_result_t sdcard_write_blocks_async(uint32_t lba, const uint8_t *buffer, size_t count, uint32_t timeout_ms,
                                          sdcard_async_callback_t callback, void *user_data) {
    return sdcard_write_blocks_async_ex(lba, buffer, count, timeout_ms, 0, callback, user_data);
}

sdcard_result_t sdcard_write_blocks_async_ex(uint32_t lba, const uint8_t *buffer, size_t count, uint32_t timeout_ms,
                                             size_t progress_interval_blocks, sdcard_async_callback_t callback, void *user_data) {
    return async_submit(ASYNC_OPERATION_WRITE, lba, NULL, buffer, count, timeout_ms, progress_interval_blocks, callback, user_data);
}

bool sdcard_async_busy(void) {
    return async_is_busy();
}

sdcard_result_t sdcard_async_cancel(void) {
    if (driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED) {
        return SDCARD_ERROR_NOT_READY;
    }
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    if (!driver_state.async.initialized) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_NOT_READY;
    }
    if (driver_state.arbitration.owner != SDCARD_OWNER_ASYNC || driver_state.async.phase == ASYNC_PHASE_IDLE) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_NOT_READY;
    }
    if (driver_state.async.phase == ASYNC_PHASE_COMPLETION_PENDING) {
        critical_section_exit(&driver_state.arbitration.lock);
        return SDCARD_ERROR_BUSY;
    }
    driver_state.async.cancel_requested = true;
    critical_section_exit(&driver_state.arbitration.lock);
    __sev();
    return SDCARD_OK;
}

/* -------------------------------------------------------------------------- */
/* Callback dispatch and cooperative scheduler                                */
/* -------------------------------------------------------------------------- */

static void async_dispatch_completion(void) {
    sdcard_async_callback_t callback;
    sdcard_async_event_flags_t events;
    sdcard_result_t result;
    size_t transferred;
    void *user_data;
    sdcard_owner_t owner;

    critical_section_enter_blocking(&driver_state.arbitration.lock);
    owner = driver_state.arbitration.owner;
    hard_assert(owner == SDCARD_OWNER_ASYNC || (owner == SDCARD_OWNER_BLOCKING && driver_state.async.request.blocking_facade));
    callback = driver_state.async.request.callback;
    events = driver_state.async.pending_callback_events;
    result = driver_state.async.request.result;
    transferred = driver_state.async.request.transferred;
    user_data = driver_state.async.request.user_data;
    driver_state.async.phase = ASYNC_PHASE_IDLE;
    driver_state.async.pending_callback_events = 0;
    driver_state.async.cancel_requested = false;
    if (owner == SDCARD_OWNER_ASYNC) {
        driver_state.arbitration.owner = SDCARD_OWNER_IDLE;
    }
    critical_section_exit(&driver_state.arbitration.lock);
    __sev();
    callback(events, result, transferred, user_data);
}

static void async_dispatch_progress(void) {
    sdcard_async_callback_t callback;
    sdcard_result_t result = SDCARD_OK;
    size_t transferred;
    void *user_data;
    sdcard_async_phase_t resume_phase;

    critical_section_enter_blocking(&driver_state.arbitration.lock);
    hard_assert(driver_state.arbitration.owner == SDCARD_OWNER_ASYNC);
    hard_assert(driver_state.async.phase == ASYNC_PHASE_PROGRESS_PENDING);
    callback = driver_state.async.request.callback;
    transferred = driver_state.async.request.transferred;
    user_data = driver_state.async.request.user_data;
    resume_phase = driver_state.async.progress_resume_phase;
    driver_state.async.phase = resume_phase;
    critical_section_exit(&driver_state.arbitration.lock);
    callback(SDCARD_ASYNC_EVENT_PROGRESS, result, transferred, user_data);
}

static bool async_task_gate_try_enter(void) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&driver_state.async.task_gate, &expected, ASYNC_TASK_GATE_RUNNING, false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
        return true;
    }
    /* An IRQ/alarm must never wait for the active task invocation. */
    __atomic_fetch_or(&driver_state.async.task_gate, ASYNC_TASK_GATE_PENDING, __ATOMIC_RELEASE);
    __sev();
    return false;
}

static bool async_task_gate_leave(void) {
    const uint32_t previous = __atomic_exchange_n(&driver_state.async.task_gate, 0u, __ATOMIC_RELEASE);
    const bool pending = (previous & ASYNC_TASK_GATE_PENDING) != 0;
    if (pending)
        __sev();
    return pending;
}

static sdcard_async_task_hint_t sdcard_async_task_impl(bool from_irq) {
    if (driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED) {
        return SDCARD_ASYNC_TASK_IDLE;
    }
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    const bool initialized = driver_state.async.initialized;
    const uint owner_core = driver_state.async.owner_core;
    critical_section_exit(&driver_state.arbitration.lock);
    if (!initialized)
        return SDCARD_ASYNC_TASK_IDLE;
    hard_assert(get_core_num() == owner_core);
    driver_state.async.task_from_irq_active = from_irq;

    for (unsigned step = 0; step < SDCARD_ASYNC_TASK_TRANSITION_BUDGET; ++step) {
        const sdcard_async_phase_t phase = async_get_phase();
        if (phase == ASYNC_PHASE_IDLE)
            return SDCARD_ASYNC_TASK_IDLE;
        if (phase == ASYNC_PHASE_IRQ_COMPLETION_PENDING) {
            if (from_irq)
                return SDCARD_ASYNC_TASK_IRQ_DISABLED;
            async_complete_finalize(driver_state.async.irq_deferred_result);
            continue;
        }
        if (phase == ASYNC_PHASE_PROGRESS_PENDING) {
            /* Progress callbacks keep the request owner and resume next task call. */
            if (from_irq && !driver_state.async.config.allow_callback_from_irq) {
                return SDCARD_ASYNC_TASK_IRQ_DISABLED;
            }
            async_dispatch_progress();
            return SDCARD_ASYNC_TASK_AGAIN;
        }
        if (phase == ASYNC_PHASE_COMPLETION_PENDING) {
            /* Keep completion dispatch as a task-call boundary. */
            if (from_irq && !driver_state.async.config.allow_callback_from_irq) {
                return SDCARD_ASYNC_TASK_IRQ_DISABLED;
            }
            async_dispatch_completion();
            return async_get_phase() == ASYNC_PHASE_IDLE ? SDCARD_ASYNC_TASK_IDLE : SDCARD_ASYNC_TASK_AGAIN;
        }
        if (async_cancel_was_requested()) {
            async_complete(SDCARD_ERROR_CANCELLED);
            return SDCARD_ASYNC_TASK_AGAIN;
        }
        if (async_request_timed_out()) {
            async_complete(SDCARD_ERROR_TIMEOUT);
            return SDCARD_ASYNC_TASK_AGAIN;
        }

        switch (phase) {
        case ASYNC_PHASE_QUEUED:
            driver_state.async.probe_attempts = 0;
            driver_state.async.have_mismatch = false;
            driver_state.async.have_successful_probe = false;
            async_begin_selection((driver_state.config.init_flags & SDCARD_INIT_CHECK_MEDIA_BEFORE_IO)
                                      ? ASYNC_PHASE_PRECHECK_STATUS_WAIT_READY
                                      : ASYNC_PHASE_COMMAND_WAIT_READY);
            continue;

        case ASYNC_PHASE_PRECHECK_STATUS_WAIT_READY: {
            if (!async_spi_wait_ready_bounded())
                return SDCARD_ASYNC_TASK_WAITING;
            const uint8_t r1 = async_send_command_selected(CMD13, 0);
            if (r1 == 0xFF || (r1 & 0x80u)) {
                spi_deselect_card();
                async_probe_failed();
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            (void)spi_transfer_byte(0xFF); /* Second R2 byte. */
            spi_deselect_card();
            if (r1 & 0x01u) {
                async_probe_failed();
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            async_begin_selection(ASYNC_PHASE_PRECHECK_CID_WAIT_READY);
            continue;
        }

        case ASYNC_PHASE_PRECHECK_CID_WAIT_READY: {
            if (!async_spi_wait_ready_bounded())
                return SDCARD_ASYNC_TASK_WAITING;
            const sdcard_result_t result = command_result(async_send_command_selected(CMD10, 0));
            if (result != SDCARD_OK) {
                spi_deselect_card();
                async_probe_failed();
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            async_set_phase(ASYNC_PHASE_PRECHECK_CID_WAIT_TOKEN);
            continue;
        }

        case ASYNC_PHASE_PRECHECK_CID_WAIT_TOKEN: {
            const uint8_t token = async_spi_poll_token_bounded();
            if (token == 0xFF)
                return SDCARD_ASYNC_TASK_WAITING;
            if (token != 0xFE) {
                spi_deselect_card();
                async_probe_failed();
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            spi_receive_bytes(driver_state.async.probe_candidate, sizeof(driver_state.async.probe_candidate));
            const uint16_t received_crc = (uint16_t)spi_transfer_byte(0xFF) << 8 | spi_transfer_byte(0xFF);
            if (data_crc_enabled() &&
                received_crc != crc16_ccitt(driver_state.async.probe_candidate, sizeof(driver_state.async.probe_candidate))) {
                spi_deselect_card();
                async_complete(SDCARD_ERROR_CRC);
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            spi_deselect_card();
            async_process_probe_cid();
            continue;
        }

        case ASYNC_PHASE_COMMAND_WAIT_READY: {
            if (!async_spi_wait_ready_bounded())
                return SDCARD_ASYNC_TASK_WAITING;
            const uint8_t command = driver_state.async.request.operation == ASYNC_OPERATION_READ
                                        ? (driver_state.async.request.multi_block ? CMD18 : CMD17)
                                        : (driver_state.async.request.multi_block ? CMD25 : CMD24);
            const sdcard_result_t result = command_result(async_send_command_selected(command, async_request_address()));
            if (result != SDCARD_OK) {
                async_complete(result);
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            driver_state.async.request.transfer_started = true;
            async_set_phase(driver_state.async.request.operation == ASYNC_OPERATION_READ ? ASYNC_PHASE_READ_WAIT_TOKEN
                                                                                         : ASYNC_PHASE_WRITE_WAIT_READY);
            continue;
        }

        case ASYNC_PHASE_READ_WAIT_TOKEN: {
            const uint8_t token = async_spi_poll_token_bounded();
            if (token == 0xFF)
                return SDCARD_ASYNC_TASK_WAITING;
            if (token != 0xFE) {
                async_complete(SDCARD_ERROR_DATA_TOKEN);
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            const size_t dma_block =
                async_crc_pipeline_enabled() ? driver_state.async.request.read_dma_block_index : driver_state.async.request.transferred;
            async_start_read_dma(driver_state.async.request.read_buffer + dma_block * SDCARD_BLOCK_SIZE);
            if (async_crc_pipeline_enabled() && dma_block != 0) {
                /* DMA is receiving sector N while the CPU validates N-1. */
                hard_assert(driver_state.async.request.read_pending_crc_valid);
                const uint8_t *previous_block = driver_state.async.request.read_buffer + (dma_block - 1u) * SDCARD_BLOCK_SIZE;
                const uint16_t expected_crc = crc16_ccitt(previous_block, SDCARD_BLOCK_SIZE);
                if (driver_state.async.request.read_pending_crc != expected_crc) {
                    driver_state.async.request.read_pending_crc_valid = false;
                    async_complete(SDCARD_ERROR_CRC);
                    return SDCARD_ASYNC_TASK_AGAIN;
                }
                driver_state.async.request.read_pending_crc_valid = false;
                ++driver_state.async.request.transferred;
                if (async_progress_threshold_reached() && driver_state.async.request.transferred < driver_state.async.request.count) {
                    driver_state.async.request.read_progress_pending = true;
                }
            }
            async_set_phase(ASYNC_PHASE_READ_DMA);
            continue;
        }

        case ASYNC_PHASE_READ_DMA: {
            const async_dma_payload_state_t dma_state = async_dma_payload_state();
            if (dma_state == ASYNC_DMA_WAITING)
                return SDCARD_ASYNC_TASK_WAITING;
            if (dma_state == ASYNC_DMA_ERROR) {
                async_complete(SDCARD_ERROR_IO);
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            driver_state.async.request.dma_active = false;
            async_set_phase(ASYNC_PHASE_READ_DRAIN);
            continue;
        }

        case ASYNC_PHASE_READ_DRAIN:
            if (spi_is_busy(SDCARD_SPI_BUS))
                return SDCARD_ASYNC_TASK_WAITING;
            if (async_crc_pipeline_enabled() && driver_state.async.request.read_progress_pending) {
                /* Do not expose progress while the next sector DMA is active. */
                driver_state.async.request.read_progress_pending = false;
                if (async_queue_progress(ASYNC_PHASE_READ_DRAIN)) {
                    return SDCARD_ASYNC_TASK_AGAIN;
                }
            }
            {
                uint16_t received_crc = (uint16_t)spi_transfer_byte(0xFF) << 8 | spi_transfer_byte(0xFF);
                if (crc_test_take_data_corruption())
                    received_crc ^= 1u;
                const size_t dma_block =
                    async_crc_pipeline_enabled() ? driver_state.async.request.read_dma_block_index : driver_state.async.request.transferred;
                const uint8_t *block = driver_state.async.request.read_buffer + dma_block * SDCARD_BLOCK_SIZE;
                if (async_crc_pipeline_enabled() && dma_block + 1u < driver_state.async.request.count) {
                    driver_state.async.request.read_pending_crc = received_crc;
                    driver_state.async.request.read_pending_crc_valid = true;
                    ++driver_state.async.request.read_dma_block_index;
                    async_set_phase(ASYNC_PHASE_READ_WAIT_TOKEN);
                    continue;
                }
                if (data_crc_enabled() && received_crc != crc16_ccitt(block, SDCARD_BLOCK_SIZE)) {
                    async_complete(SDCARD_ERROR_CRC);
                    return SDCARD_ASYNC_TASK_AGAIN;
                }
            }
            ++driver_state.async.request.transferred;
            if (driver_state.async.request.transferred < driver_state.async.request.count) {
                if (async_queue_progress(ASYNC_PHASE_READ_WAIT_TOKEN)) {
                    return SDCARD_ASYNC_TASK_AGAIN;
                }
                async_set_phase(ASYNC_PHASE_READ_WAIT_TOKEN);
                continue;
            }
            if (driver_state.async.request.multi_block) {
                const sdcard_result_t stop_result = command_result(async_send_command_selected(CMD12, 0));
                if (stop_result != SDCARD_OK) {
                    async_complete(stop_result);
                    return SDCARD_ASYNC_TASK_AGAIN;
                }
                driver_state.async.request.transfer_started = false;
            }
            async_complete(SDCARD_OK);
            return SDCARD_ASYNC_TASK_AGAIN;

        case ASYNC_PHASE_WRITE_WAIT_READY:
            if (!async_spi_wait_ready_bounded())
                return SDCARD_ASYNC_TASK_WAITING;
            if (async_crc_pipeline_enabled() && driver_state.async.request.next_data_crc_valid) {
                driver_state.async.request.data_crc = driver_state.async.request.next_data_crc;
                driver_state.async.request.next_data_crc_valid = false;
            } else {
                driver_state.async.request.data_crc =
                    data_crc_enabled()
                        ? crc16_ccitt(driver_state.async.request.write_buffer + driver_state.async.request.transferred * SDCARD_BLOCK_SIZE,
                                      SDCARD_BLOCK_SIZE)
                        : 0xFFFFu;
            }
            (void)spi_transfer_byte(driver_state.async.request.multi_block ? 0xFC : 0xFE);
            async_start_write_dma(driver_state.async.request.write_buffer + driver_state.async.request.transferred * SDCARD_BLOCK_SIZE);
            if (async_crc_pipeline_enabled() && driver_state.async.request.transferred + 1u < driver_state.async.request.count) {
                /* Prepare the next packet's CRC during this packet's DMA. */
                driver_state.async.request.next_data_crc =
                    crc16_ccitt(driver_state.async.request.write_buffer + (driver_state.async.request.transferred + 1u) * SDCARD_BLOCK_SIZE,
                                SDCARD_BLOCK_SIZE);
                driver_state.async.request.next_data_crc_valid = true;
            }
            async_set_phase(ASYNC_PHASE_WRITE_DMA);
            continue;

        case ASYNC_PHASE_WRITE_DMA: {
            const async_dma_payload_state_t dma_state = async_dma_payload_state();
            if (dma_state == ASYNC_DMA_WAITING)
                return SDCARD_ASYNC_TASK_WAITING;
            if (dma_state == ASYNC_DMA_ERROR) {
                async_complete(SDCARD_ERROR_IO);
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            driver_state.async.request.dma_active = false;
            async_set_phase(ASYNC_PHASE_WRITE_DRAIN);
            continue;
        }

        case ASYNC_PHASE_WRITE_DRAIN:
            if (spi_is_busy(SDCARD_SPI_BUS))
                return SDCARD_ASYNC_TASK_WAITING;
            (void)spi_transfer_byte((uint8_t)(driver_state.async.request.data_crc >> 8));
            (void)spi_transfer_byte((uint8_t)driver_state.async.request.data_crc);
            async_set_phase(ASYNC_PHASE_WRITE_RESPONSE);
            continue;

        case ASYNC_PHASE_WRITE_RESPONSE: {
            const uint8_t response = spi_transfer_byte(0xFF) & 0x1Fu;
            if (response == 0x0Bu) {
                async_complete(SDCARD_ERROR_CRC);
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            if (response != 0x05u) {
                async_complete(SDCARD_ERROR_IO);
                return SDCARD_ASYNC_TASK_AGAIN;
            }
            async_set_phase(ASYNC_PHASE_WRITE_WAIT_BUSY);
            continue;
        }

        case ASYNC_PHASE_WRITE_WAIT_BUSY:
            if (!async_spi_wait_ready_bounded())
                return SDCARD_ASYNC_TASK_WAITING;
            ++driver_state.async.request.transferred;
            if (driver_state.async.request.transferred < driver_state.async.request.count) {
                if (async_queue_progress(ASYNC_PHASE_WRITE_WAIT_READY)) {
                    return SDCARD_ASYNC_TASK_AGAIN;
                }
                async_set_phase(ASYNC_PHASE_WRITE_WAIT_READY);
                continue;
            }
            if (driver_state.async.request.multi_block) {
                (void)spi_transfer_byte(0xFD);
                async_set_phase(ASYNC_PHASE_WRITE_WAIT_FINAL_BUSY);
                continue;
            }
            async_complete(SDCARD_OK);
            return SDCARD_ASYNC_TASK_AGAIN;

        case ASYNC_PHASE_WRITE_WAIT_FINAL_BUSY:
            if (!async_spi_wait_ready_bounded())
                return SDCARD_ASYNC_TASK_WAITING;
            driver_state.async.request.transfer_started = false;
            async_complete(SDCARD_OK);
            return SDCARD_ASYNC_TASK_AGAIN;

        default:
            async_complete(SDCARD_ERROR_IO);
            return SDCARD_ASYNC_TASK_AGAIN;
        }
    }
    return SDCARD_ASYNC_TASK_AGAIN;
}

sdcard_async_task_hint_t sdcard_async_task(void) {
    if (!async_task_gate_try_enter())
        return SDCARD_ASYNC_TASK_AGAIN;
    const sdcard_async_task_hint_t hint = sdcard_async_task_impl(false);
    return async_task_gate_leave() ? SDCARD_ASYNC_TASK_AGAIN : hint;
}

static bool async_irq_entry_allowed(void) {
    if (driver_state.arbitration.owner == SDCARD_OWNER_UNINITIALIZED)
        return false;
    critical_section_enter_blocking(&driver_state.arbitration.lock);
    const sdcard_async_phase_t phase = driver_state.async.phase;
    const bool allowed =
        driver_state.async.initialized && (driver_state.async.config.allow_task_from_irq ||
                                           (driver_state.async.config.allow_callback_from_irq &&
                                            (phase == ASYNC_PHASE_PROGRESS_PENDING || phase == ASYNC_PHASE_COMPLETION_PENDING)));
    critical_section_exit(&driver_state.arbitration.lock);
    return allowed;
}

sdcard_async_task_hint_t sdcard_async_task_from_irq(void) {
    if (!async_irq_entry_allowed())
        return SDCARD_ASYNC_TASK_IRQ_DISABLED;
    if (!async_task_gate_try_enter())
        return SDCARD_ASYNC_TASK_AGAIN;
    const sdcard_async_task_hint_t hint = sdcard_async_task_impl(true);
    return async_task_gate_leave() ? SDCARD_ASYNC_TASK_AGAIN : hint;
}

/* -------------------------------------------------------------------------- */
/* Public string helpers                                                      */
/* -------------------------------------------------------------------------- */

const char *sdcard_result_string(sdcard_result_t result) {
    switch (result) {
    case SDCARD_OK:
        return "ok";
    case SDCARD_ERROR_IO:
        return "I/O error";
    case SDCARD_ERROR_NOT_READY:
        return "not ready";
    case SDCARD_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case SDCARD_ERROR_WRITE_PROTECTED:
        return "write protected";
    case SDCARD_ERROR_UNSUPPORTED:
        return "unsupported";
    case SDCARD_ERROR_TIMEOUT:
        return "timeout";
    case SDCARD_ERROR_COMMAND_REJECTED:
        return "command rejected";
    case SDCARD_ERROR_CRC:
        return "CRC error";
    case SDCARD_ERROR_DATA_TOKEN:
        return "invalid data token";
    case SDCARD_ERROR_OUT_OF_RANGE:
        return "out of range";
    case SDCARD_ERROR_LOCKED:
        return "card locked";
    case SDCARD_ERROR_MEDIA_CHANGED:
        return "media changed";
    case SDCARD_ERROR_BUSY:
        return "busy";
    case SDCARD_ERROR_CANCELLED:
        return "cancelled";
    default:
        return "unknown error";
    }
}

const char *sdcard_type_string(sdcard_type_t type) {
    switch (type) {
    case SDCARD_TYPE_MMC_V3:
        return "MMC v3";
    case SDCARD_TYPE_SD_V1:
        return "SD v1";
    case SDCARD_TYPE_SD_V2:
        return "SD v2 (byte addressed)";
    case SDCARD_TYPE_SDHC_SDXC:
        return "SDHC/SDXC (block addressed)";
    default:
        return "unknown";
    }
}
