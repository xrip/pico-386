/**
 * @file sdcard_async.h
 * @brief Blocking and cooperative asynchronous filesystem-independent SPI SD card interface.
 *
 * Only one card operation may own the driver. Competing blocking and async
 * calls return BUSY instead of waiting. After the initial single-threaded
 * sdcard_init(), operation arbitration, async submission, status, and cancel
 * are safe across RP2350 cores and regular IRQ context. All addresses use
 * logical 512-byte sectors. Register bitfield views require the little-endian
 * GNU-compatible ARM ABI.
 */

#ifndef SDCARD_ASYNC_H
#define SDCARD_ASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hardware/spi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Logical block size exposed by the driver, in bytes. */
#define SDCARD_BLOCK_SIZE 512u

/** Pico SDK SPI instance used by the card. */
#ifndef SDCARD_SPI_BUS
#define SDCARD_SPI_BUS spi0
#endif

/** GPIO connected to the card CS input. */
#ifndef SDCARD_PIN_SPI0_CS
#define SDCARD_PIN_SPI0_CS 22
#endif

/** GPIO connected to the card SCK input. */
#ifndef SDCARD_PIN_SPI0_SCK
#define SDCARD_PIN_SPI0_SCK 18
#endif

/** GPIO connected to the card MOSI input. */
#ifndef SDCARD_PIN_SPI0_MOSI
#define SDCARD_PIN_SPI0_MOSI 19
#endif

/** GPIO connected to the card MISO output. */
#ifndef SDCARD_PIN_SPI0_MISO
#define SDCARD_PIN_SPI0_MISO 16
#endif

/** Requested SPI baud rate used during card initialization, in Hz. */
#ifndef SDCARD_SPI_INITIAL_BAUD_RATE
#define SDCARD_SPI_INITIAL_BAUD_RATE 100000u
#endif

/** Requested SPI baud rate used after card initialization, in Hz. */
#ifndef SDCARD_SPI_BAUD_RATE
#define SDCARD_SPI_BAUD_RATE 25000000u
#endif

/** Compile-time CRC16 implementation selector: bitwise, nibble-table or byte-table. */
#define SDCARD_CRC16_IMPL_BITWISE 0
#define SDCARD_CRC16_IMPL_NIBBLE 1
#define SDCARD_CRC16_IMPL_BYTE 2
#ifndef SDCARD_CRC16_IMPL
#define SDCARD_CRC16_IMPL SDCARD_CRC16_IMPL_BYTE
#endif
#if SDCARD_CRC16_IMPL < SDCARD_CRC16_IMPL_BITWISE || SDCARD_CRC16_IMPL > SDCARD_CRC16_IMPL_BYTE
#error "SDCARD_CRC16_IMPL must be bitwise (0), nibble-table (1) or byte-table (2)"
#endif

/** Overlap async DMA payload transfers with software CRC16 calculation. */
#ifndef SDCARD_ASYNC_CRC_PIPELINE
#define SDCARD_ASYNC_CRC_PIPELINE 1
#endif
#if SDCARD_ASYNC_CRC_PIPELINE != 0 && SDCARD_ASYNC_CRC_PIPELINE != 1
#error "SDCARD_ASYNC_CRC_PIPELINE must be 0 or 1"
#endif

/** Collect DMA IRQ latency and error counters. Set to zero for production builds. */
#ifndef SDCARD_ASYNC_ENABLE_STATS
#define SDCARD_ASYNC_ENABLE_STATS 1
#endif
#if SDCARD_ASYNC_ENABLE_STATS != 0 && SDCARD_ASYNC_ENABLE_STATS != 1
#error "SDCARD_ASYNC_ENABLE_STATS must be 0 or 1"
#endif

#ifdef SDCARD_CRC_TEST_HOOK
/**
 * @brief Validate CRC7 and CRC16 vectors for the selected build variant.
 * @return true when every deterministic vector matches.
 * @note Available only in CRC validation test builds.
 */
bool sdcard_crc_test_vectors(void);

/**
 * @brief Arm the test hook that corrupts one received data CRC.
 * @param successful_packets Number of valid packets to pass first.
 * @note Available only in CRC validation test builds.
 */
void sdcard_crc_test_corrupt_data_crc_after(uint32_t successful_packets);
#endif

#if !defined(__GNUC__)
#error "SD card register bitfields require the GNU-compatible ARM ABI"
#endif

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "SD card register bitfields require a little-endian target"
#endif

#define SDCARD_PACKED __attribute__((packed))

/** Result of a blocking SD card operation. */
typedef enum {
    SDCARD_OK = 0,                 /**< Operation completed successfully. */
    SDCARD_ERROR_IO,               /**< Card command or data transfer failed. */
    SDCARD_ERROR_NOT_READY,        /**< Card is not initialized or did not become ready. */
    SDCARD_ERROR_INVALID_ARGUMENT, /**< A pointer, count, or address is invalid. */
    SDCARD_ERROR_WRITE_PROTECTED,  /**< The requested write is prohibited. */
    SDCARD_ERROR_UNSUPPORTED,      /**< The card or requested operation is unsupported. */
    SDCARD_ERROR_TIMEOUT,          /**< The card did not respond before the deadline. */
    SDCARD_ERROR_COMMAND_REJECTED, /**< The card rejected a syntactically valid command. */
    SDCARD_ERROR_CRC,              /**< The card reported a command or data CRC error. */
    SDCARD_ERROR_DATA_TOKEN,       /**< The card returned an unexpected data token. */
    SDCARD_ERROR_OUT_OF_RANGE,     /**< A logical sector lies outside card capacity. */
    SDCARD_ERROR_LOCKED,           /**< The card is password locked. */
    SDCARD_ERROR_MEDIA_CHANGED,    /**< The initialized medium was removed or replaced. */
    SDCARD_ERROR_BUSY,             /**< Another operation currently owns the driver. */
    SDCARD_ERROR_CANCELLED,        /**< An asynchronous request was cancelled. */
} sdcard_result_t;

/** Flags controlling initialization and subsequent blocking operations. */
typedef enum {
    SDCARD_INIT_NONE = 0,                        /**< Do not perform automatic media checks. */
    SDCARD_INIT_CHECK_MEDIA_BEFORE_IO = 1u << 0, /**< Check CMD13 and CID before each I/O. */
    SDCARD_INIT_ENABLE_CRC = 1u << 1,            /**< Enable command/data CRC and verify data packets. */
} sdcard_init_flags_t;

/** Current software-detected media state. */
typedef enum {
    SDCARD_MEDIA_UNINITIALIZED = 0, /**< Driver or card has not been initialized. */
    SDCARD_MEDIA_READY,             /**< Initialized medium matches the saved CID. */
    SDCARD_MEDIA_CHANGED,           /**< Removal or replacement is latched. */
    SDCARD_MEDIA_AVAILABLE,         /**< A card answers CMD0 and requires initialization. */
} sdcard_media_state_t;

/** Transition reported by sdcard_check_media(). */
typedef enum {
    SDCARD_MEDIA_EVENT_NONE = 0,  /**< No state transition was observed. */
    SDCARD_MEDIA_EVENT_REMOVED,   /**< The initialized card stopped responding. */
    SDCARD_MEDIA_EVENT_CHANGED,   /**< A stable CID different from the saved CID was read. */
    SDCARD_MEDIA_EVENT_AVAILABLE, /**< A card is present and awaits initialization. */
} sdcard_media_event_t;

/** Card generation and addressing mode detected during initialization. */
typedef enum {
    SDCARD_TYPE_UNKNOWN = 0, /**< No recognized card type. */
    SDCARD_TYPE_MMC_V3,      /**< MMC version 3, byte addressed. */
    SDCARD_TYPE_SD_V1,       /**< SD version 1, byte addressed. */
    SDCARD_TYPE_SD_V2,       /**< SD version 2, byte addressed. */
    SDCARD_TYPE_SDHC_SDXC,   /**< SDHC or SDXC, block addressed. */
} sdcard_type_t;

/** Access mode selected through SD CMD6 function group 1. */
typedef enum {
    SDCARD_SPEED_DEFAULT = 0, /**< Default-speed mode, up to the CSD limit. */
    SDCARD_SPEED_HIGH = 1,    /**< High-speed mode, up to 50 MHz in SPI mode. */
} sdcard_speed_mode_t;

/** First byte of the SPI R2 card-status response. */
typedef union {
    uint8_t raw; /**< Raw R1-compatible status byte. */
    struct SDCARD_PACKED {
        uint8_t idle : 1;                 /**< Card is in idle state. */
        uint8_t erase_reset : 1;          /**< Erase sequence was cleared. */
        uint8_t illegal_command : 1;      /**< Command is not supported. */
        uint8_t command_crc_error : 1;    /**< Command CRC check failed. */
        uint8_t erase_sequence_error : 1; /**< Erase command sequence is invalid. */
        uint8_t address_error : 1;        /**< Address is invalid or misaligned. */
        uint8_t parameter_error : 1;      /**< Command parameter is invalid. */
        uint8_t reserved : 1;             /**< Always zero in a valid response. */
    } bits;
} sdcard_r1_status_t;

/** Second byte of the SPI R2 card-status response. */
typedef union {
    uint8_t raw; /**< Raw extended status byte. */
    struct SDCARD_PACKED {
        uint8_t card_locked : 1;             /**< Card is password locked. */
        uint8_t wp_erase_skip : 1;           /**< WP erase skipped or lock command failed. */
        uint8_t generic_error : 1;           /**< Unspecified card error. */
        uint8_t controller_error : 1;        /**< Internal controller error. */
        uint8_t card_ecc_failed : 1;         /**< Internal ECC correction failed. */
        uint8_t write_protect_violation : 1; /**< Write targeted protected data. */
        uint8_t erase_parameter : 1;         /**< Erase range or alignment is invalid. */
        uint8_t out_of_range : 1;            /**< Address exceeds capacity or CSD overwrite. */
    } bits;
} sdcard_r2_status_t;

/** Two-byte status returned by CMD13 in SPI mode. */
typedef struct {
    sdcard_r1_status_t r1; /**< Command-status byte. */
    sdcard_r2_status_t r2; /**< Extended card-status byte. */
} sdcard_status_t;

/** SD CID fields that do not cross byte boundaries. */
typedef struct SDCARD_PACKED {
    uint8_t manufacturer_id;              /**< Manufacturer identifier (MID). */
    uint8_t opaque_oem_id[2];             /**< Wire bytes occupied by OID. */
    uint8_t opaque_product_name[5];       /**< Wire bytes occupied by PNM. */
    uint8_t revision_minor : 4;           /**< Minor product revision. */
    uint8_t revision_major : 4;           /**< Major product revision. */
    uint8_t opaque_serial_number[4];      /**< Wire bytes occupied by PSN. */
    uint8_t opaque_manufacturing_date[2]; /**< Wire bytes occupied by MDT. */
    uint8_t end_bit : 1;                  /**< Fixed register end bit. */
    uint8_t crc7 : 7;                     /**< CID CRC7 value. */
} sdcard_sd_cid_bits_t;

/** MMC v3 CID fields that do not cross byte boundaries. */
typedef struct SDCARD_PACKED {
    uint8_t manufacturer_id;               /**< Manufacturer identifier (MID). */
    uint8_t opaque_oem_id[2];              /**< Wire bytes occupied by OID. */
    uint8_t opaque_product_name[6];        /**< Wire bytes occupied by PNM. */
    uint8_t revision_minor : 4;            /**< Minor product revision. */
    uint8_t revision_major : 4;            /**< Major product revision. */
    uint8_t opaque_serial_number[4];       /**< Wire bytes occupied by PSN. */
    uint8_t manufacturing_month : 4;       /**< Manufacturing month. */
    uint8_t manufacturing_year_offset : 4; /**< Year offset from 1997. */
    uint8_t end_bit : 1;                   /**< Fixed register end bit. */
    uint8_t crc7 : 7;                      /**< CID CRC7 value. */
} sdcard_mmc_cid_bits_t;

/** CID wire image with card-type-specific bitfield views. */
typedef union {
    uint8_t raw[16];           /**< CID bytes in transmission order. */
    sdcard_sd_cid_bits_t sd;   /**< SD CID layout. */
    sdcard_mmc_cid_bits_t mmc; /**< MMC v3 CID layout. */
} sdcard_cid_register_t;

/** CID register and decoded multi-byte fields. */
typedef struct {
    sdcard_cid_register_t reg;   /**< Raw register and byte-local fields. */
    char oem_id[3];              /**< Two-character OID followed by NUL. */
    char product_name[7];        /**< Product name followed by NUL. */
    uint32_t serial_number;      /**< Product serial number (PSN). */
    uint16_t manufacturing_year; /**< Four-digit manufacturing year. */
    uint8_t manufacturing_month; /**< Manufacturing month, 1 through 12. */
} sdcard_cid_t;

/** Common CSD fields whose positions do not depend on CSD structure version. */
typedef struct SDCARD_PACKED {
    uint8_t reserved_121_120 : 2;               /**< Reserved CSD bits 121:120. */
    uint8_t spec_version : 4;                   /**< MMC SPEC_VERS; reserved for SD. */
    uint8_t structure : 2;                      /**< CSD_STRUCTURE field. */
    uint8_t data_access_time;                   /**< Raw TAAC field. */
    uint8_t data_access_clocks;                 /**< Raw NSAC field. */
    uint8_t transfer_speed;                     /**< Raw TRAN_SPEED field. */
    uint8_t opaque_command_classes_high;        /**< Upper wire byte of CCC. */
    uint8_t read_block_length : 4;              /**< READ_BL_LEN exponent. */
    uint8_t opaque_command_classes_low : 4;     /**< Lower wire fragment of CCC. */
    uint8_t opaque_capacity_high : 4;           /**< Version-dependent capacity bits. */
    uint8_t dsr_implemented : 1;                /**< DSR_IMP flag. */
    uint8_t read_misaligned : 1;                /**< READ_BLK_MISALIGN flag. */
    uint8_t write_misaligned : 1;               /**< WRITE_BLK_MISALIGN flag. */
    uint8_t read_partial : 1;                   /**< READ_BL_PARTIAL flag. */
    uint8_t opaque_capacity[3];                 /**< Version-dependent capacity bytes. */
    uint8_t opaque_erase_size_high : 6;         /**< Upper wire fragment of SECTOR_SIZE. */
    uint8_t erase_single_block_enabled : 1;     /**< ERASE_BLK_EN flag. */
    uint8_t opaque_version_bit : 1;             /**< Version-dependent bit 47. */
    uint8_t write_protect_group_size : 7;       /**< WP_GRP_SIZE field. */
    uint8_t opaque_erase_size_low : 1;          /**< Lower wire fragment of SECTOR_SIZE. */
    uint8_t opaque_write_block_length_high : 2; /**< Upper WRITE_BL_LEN fragment. */
    uint8_t write_speed_factor : 3;             /**< R2W_FACTOR exponent. */
    uint8_t reserved_30_29 : 2;                 /**< Reserved CSD bits 30:29. */
    uint8_t write_protect_group_enabled : 1;    /**< WP_GRP_ENABLE flag. */
    uint8_t reserved_20_16 : 5;                 /**< Reserved CSD bits 20:16. */
    uint8_t write_partial : 1;                  /**< WRITE_BL_PARTIAL flag. */
    uint8_t opaque_write_block_length_low : 2;  /**< Lower WRITE_BL_LEN fragment. */
    uint8_t reserved_9_8 : 2;                   /**< Reserved CSD bits 9:8. */
    uint8_t file_format : 2;                    /**< FILE_FORMAT field. */
    uint8_t temporary_write_protect : 1;        /**< TMP_WRITE_PROTECT flag. */
    uint8_t permanent_write_protect : 1;        /**< PERM_WRITE_PROTECT flag. */
    uint8_t copy : 1;                           /**< COPY flag. */
    uint8_t file_format_group : 1;              /**< FILE_FORMAT_GRP flag. */
    uint8_t end_bit : 1;                        /**< Fixed register end bit. */
    uint8_t crc7 : 7;                           /**< CSD CRC7 value. */
} sdcard_csd_bits_t;

/** CSD wire image with a view of byte-local fields. */
typedef union {
    uint8_t raw[16];        /**< CSD bytes in transmission order. */
    sdcard_csd_bits_t bits; /**< Fields common to supported CSD versions. */
} sdcard_csd_register_t;

/** CSD register and decoded multi-byte or calculated fields. */
typedef struct {
    sdcard_csd_register_t reg;     /**< Raw register and byte-local fields. */
    uint16_t command_classes;      /**< Supported card command classes (CCC). */
    uint8_t write_block_length;    /**< WRITE_BL_LEN as a base-2 byte exponent. */
    uint32_t data_access_time_ns;  /**< TAAC decoded to nanoseconds. */
    uint32_t data_access_clocks;   /**< NSAC decoded to additional clock cycles. */
    uint32_t max_transfer_rate_hz; /**< TRAN_SPEED decoded to SPI bit rate in Hz. */
    uint32_t erase_sector_size;    /**< CSD erase granularity in logical sectors. */
    uint64_t capacity_bytes;       /**< Decoded user capacity in bytes. */
    uint64_t sector_count;         /**< Decoded count of logical sectors. */
} sdcard_csd_t;

/** OCR value with a host-order bitfield view. */
typedef union {
    uint32_t raw; /**< Complete OCR value in host byte order. */
    struct SDCARD_PACKED {
        uint32_t reserved_14_0 : 15;         /**< Reserved OCR bits 14:0. */
        uint32_t voltage_window : 9;         /**< Voltage window bits 23:15. */
        uint32_t switching_1v8_accepted : 1; /**< S18A status bit. */
        uint32_t reserved_28_25 : 4;         /**< Reserved OCR bits 28:25. */
        uint32_t uhs2_status : 1;            /**< UHS-II status bit. */
        uint32_t high_capacity : 1;          /**< Card Capacity Status bit. */
        uint32_t power_up_complete : 1;      /**< Power-up status bit. */
    } bits;
} sdcard_ocr_t;

/** SCR fields that do not cross byte boundaries. */
typedef struct SDCARD_PACKED {
    uint8_t sd_spec : 4;           /**< SD_SPEC revision field. */
    uint8_t structure : 4;         /**< SCR_STRUCTURE revision. */
    uint8_t bus_widths : 4;        /**< Supported SD bus widths. */
    uint8_t security : 3;          /**< CPRM security revision. */
    uint8_t data_after_erase : 1;  /**< Erased data status. */
    uint8_t opaque_specx_high : 2; /**< Upper wire fragment of SD_SPECX. */
    uint8_t spec4 : 1;             /**< SD_SPEC4 revision flag. */
    uint8_t extended_security : 4; /**< Extended security support. */
    uint8_t spec3 : 1;             /**< SD_SPEC3 revision flag. */
    uint8_t command_support : 4;   /**< Optional command support flags. */
    uint8_t reserved_37_36 : 2;    /**< Reserved SCR bits 37:36. */
    uint8_t opaque_specx_low : 2;  /**< Lower wire fragment of SD_SPECX. */
    uint8_t manufacturer[4];       /**< Manufacturer-reserved SCR bits. */
} sdcard_scr_bits_t;

/** SCR wire image with a view of byte-local fields. */
typedef union {
    uint8_t raw[8];         /**< SCR bytes in transmission order. */
    sdcard_scr_bits_t bits; /**< Byte-local SCR fields. */
} sdcard_scr_register_t;

/** SCR register and decoded cross-byte fields. */
typedef struct {
    sdcard_scr_register_t reg; /**< Raw register and byte-local fields. */
    uint8_t specx;             /**< Combined SD_SPECX field. */
} sdcard_scr_t;

/** SD Status fields that do not cross byte boundaries. */
typedef struct SDCARD_PACKED {
    uint8_t reserved_506_504 : 5;         /**< Reserved status bits 506:504. */
    uint8_t secured_mode : 1;             /**< Card is in secured mode. */
    uint8_t bus_width : 2;                /**< Current DAT bus width encoding. */
    uint8_t reserved_503_496;             /**< Reserved status bits 503:496. */
    uint8_t opaque_card_type[2];          /**< Wire bytes occupied by CARD_TYPE. */
    uint8_t opaque_protected_area[4];     /**< Wire bytes occupied by protected size. */
    uint8_t speed_class;                  /**< SD speed-class code. */
    uint8_t performance_move;             /**< Sustained move performance, MB/s. */
    uint8_t reserved_435_432 : 4;         /**< Reserved status bits 435:432. */
    uint8_t allocation_unit_size : 4;     /**< AU_SIZE encoding. */
    uint8_t opaque_erase_size[2];         /**< Wire bytes occupied by ERASE_SIZE. */
    uint8_t erase_offset : 2;             /**< Erase offset encoding. */
    uint8_t erase_timeout : 6;            /**< Erase timeout encoding. */
    uint8_t uhs_allocation_unit_size : 4; /**< UHS_AU_SIZE encoding. */
    uint8_t uhs_speed_grade : 4;          /**< UHS_SPEED_GRADE encoding. */
    uint8_t video_speed_class;            /**< Video speed-class code. */
    uint8_t trailing[48];                 /**< Remaining SD Status bytes. */
} sdcard_sd_status_bits_t;

/** 512-bit SD Status wire image. */
typedef union {
    uint8_t raw[64];              /**< Status bytes in transmission order. */
    sdcard_sd_status_bits_t bits; /**< Byte-local status fields. */
} sdcard_sd_status_register_t;

/** SD Status and decoded multi-byte or calculated fields. */
typedef struct {
    sdcard_sd_status_register_t reg;     /**< Raw status and byte-local fields. */
    uint64_t allocation_unit_size_bytes; /**< Decoded allocation-unit size. */
    uint32_t protected_area_size_raw;    /**< Raw SIZE_OF_PROTECTED_AREA field. */
    uint16_t card_type;                  /**< CARD_TYPE field. */
    uint16_t erase_size;                 /**< Erase size in allocation units. */
} sdcard_sd_status_t;

/** Relevant fields from the 512-bit CMD6 switch-function status. */
typedef struct {
    uint8_t raw[64];              /**< Complete status in transmission order. */
    uint16_t max_current_ma;      /**< Maximum current consumption, in mA. */
    uint16_t access_mode_support; /**< Function-group 1 support bitmap. */
    uint8_t selected_access_mode; /**< Function selected for group 1. */
} sdcard_switch_status_t;

#if defined(__cplusplus)
static_assert(sizeof(sdcard_sd_cid_bits_t) == 16, "Invalid SD CID bitfield layout");
static_assert(sizeof(sdcard_mmc_cid_bits_t) == 16, "Invalid MMC CID bitfield layout");
static_assert(sizeof(sdcard_csd_bits_t) == 16, "Invalid CSD bitfield layout");
static_assert(sizeof(sdcard_ocr_t) == 4, "Invalid OCR bitfield layout");
static_assert(sizeof(sdcard_r1_status_t) == 1, "Invalid R1 bitfield layout");
static_assert(sizeof(sdcard_r2_status_t) == 1, "Invalid R2 bitfield layout");
static_assert(sizeof(sdcard_scr_bits_t) == 8, "Invalid SCR bitfield layout");
static_assert(sizeof(sdcard_sd_status_bits_t) == 64, "Invalid SD Status bitfield layout");
#else
_Static_assert(sizeof(sdcard_sd_cid_bits_t) == 16, "Invalid SD CID bitfield layout");
_Static_assert(sizeof(sdcard_mmc_cid_bits_t) == 16, "Invalid MMC CID bitfield layout");
_Static_assert(sizeof(sdcard_csd_bits_t) == 16, "Invalid CSD bitfield layout");
_Static_assert(sizeof(sdcard_ocr_t) == 4, "Invalid OCR bitfield layout");
_Static_assert(sizeof(sdcard_r1_status_t) == 1, "Invalid R1 bitfield layout");
_Static_assert(sizeof(sdcard_r2_status_t) == 1, "Invalid R2 bitfield layout");
_Static_assert(sizeof(sdcard_scr_bits_t) == 8, "Invalid SCR bitfield layout");
_Static_assert(sizeof(sdcard_sd_status_bits_t) == 64, "Invalid SD Status bitfield layout");
#endif

#undef SDCARD_PACKED

/** Card type and identification registers collected in one operation. */
typedef struct {
    sdcard_type_t type; /**< Type reported by sdcard_get_type(). */
    sdcard_cid_t cid;   /**< CID register and decoded fields. */
    sdcard_csd_t csd;   /**< CSD register and decoded fields. */
    sdcard_ocr_t ocr;   /**< OCR register and decoded fields. */
} sdcard_info_t;

/** Disable the asynchronous operation deadline. */
#define SDCARD_ASYNC_TIMEOUT_INFINITE UINT32_MAX

/** Blocking data-transfer backend selected during async initialization. */
typedef enum {
    SDCARD_BLOCKING_TRANSPORT_CPU = 0, /**< Use the direct CPU/SPI path. */
    SDCARD_BLOCKING_TRANSPORT_DMA = 1, /**< Submit a private async DMA request. */
} sdcard_blocking_transport_t;

/** Default overall timeout for a DMA-backed blocking transfer, in milliseconds. */
#ifndef SDCARD_BLOCKING_TIMEOUT_DEFAULT_MS
#define SDCARD_BLOCKING_TIMEOUT_DEFAULT_MS 30000u
#endif

/** Application-owned resources used by the asynchronous transport. */
typedef struct {
    int tx_dma_channel;                             /**< Claimed DMA channel feeding the SPI TX FIFO. */
    int rx_dma_channel;                             /**< Claimed DMA channel draining the SPI RX FIFO. */
    unsigned int dma_irq_index;                     /**< Shared DMA IRQ index used when IRQ mode is enabled. */
    uint32_t blocking_timeout_ms;                   /**< Overall DMA blocking timeout; zero selects the default. */
    sdcard_blocking_transport_t blocking_transport; /**< Backend for blocking read/write calls; CPU is the default. */
    bool use_dma_irq;                               /**< Use shared DMA completion IRQs; false keeps polling fallback. */
    bool allow_task_from_irq;                       /**< Permit sdcard_async_task_from_irq() to advance protocol phases. */
    bool allow_callback_from_irq;                   /**< Permit callbacks from sdcard_async_task_from_irq(). */
} sdcard_async_config_t;

/** Transport counters collected since the last async initialization. */
typedef struct {
    uint64_t dma_irq_payloads;         /**< Payload DMA completions observed by the task. */
    uint64_t dma_irq_latency_us_total; /**< Sum of IRQ-to-task completion latencies. */
    uint32_t dma_irq_latency_us_max;   /**< Maximum IRQ-to-task completion latency. */
    uint32_t dma_errors;               /**< DMA bus errors detected during payload transfers. */
} sdcard_async_stats_t;

/** Scheduling hint returned by one bounded sdcard_async_task() call. */
typedef enum {
    SDCARD_ASYNC_TASK_IDLE = 0,     /**< No request needs service. */
    SDCARD_ASYNC_TASK_WAITING,      /**< Wait for a hardware/card event or poll later. */
    SDCARD_ASYNC_TASK_AGAIN,        /**< Immediate work remains; call task again soon. */
    SDCARD_ASYNC_TASK_IRQ_DISABLED, /**< IRQ execution is not enabled for this phase. */
} sdcard_async_task_hint_t;

/** No-RTOS wakeup information for the owner main loop. */
typedef struct {
    uint64_t deadline_us; /**< Absolute timeout from time_us_64(), when valid. */
    bool request_active;  /**< A request is queued, active or awaiting callback. */
    bool task_running;    /**< Owner task currently holds the non-reentrant gate. */
    bool deadline_valid;  /**< @ref deadline_us contains an absolute timeout. */
    bool wake_on_dma_irq; /**< Shared DMA IRQ can wake the owner for this phase. */
    bool needs_poll;      /**< Call task periodically when no DMA IRQ wakeup applies. */
} sdcard_async_schedule_t;

/** Events delivered by one asynchronous callback invocation. */
typedef enum {
    SDCARD_ASYNC_EVENT_PROGRESS = 1u << 0, /**< A completed progress interval. */
    SDCARD_ASYNC_EVENT_COMPLETE = 1u << 1, /**< The request reached its terminal state. */
} sdcard_async_event_flags_t;

/**
 * @brief Completion and optional progress callback for one async request.
 * @param events Bitwise OR of SDCARD_ASYNC_EVENT_* flags.
 * @param result Current request result; meaningful for completion events.
 * @param blocks_transferred Cumulative sectors completed at this boundary.
 * @param user_data Opaque pointer supplied at submission.
 * @note The buffer remains owned by the caller until the terminal callback.
 * @note With allow_callback_from_irq, the callback must be ISR-safe.
 */
typedef void (*sdcard_async_callback_t)(sdcard_async_event_flags_t events, sdcard_result_t result, size_t blocks_transferred,
                                        void *user_data);

/**
 * @brief Initialize the SPI bus and place the card in SPI transfer mode.
 * @param flags Bitwise OR of sdcard_init_flags_t values.
 * @return SDCARD_OK on success; otherwise an error code.
 * @note The first call initializes shared synchronization and must not race
 * with another API call.
 */
sdcard_result_t sdcard_init(sdcard_init_flags_t flags);

/**
 * @brief Wait for pending programming and release the SPI and GPIO resources.
 * @return SDCARD_OK on a clean shutdown; otherwise the final synchronization error.
 * @note The driver is left uninitialized even when synchronization fails.
 */
sdcard_result_t sdcard_deinit(void);

/**
 * @brief Check whether the initialized medium was removed or replaced.
 * @param event Optional destination for the detected state transition; may be NULL.
 * @return SDCARD_OK only while the saved medium remains ready; otherwise an error code.
 * @note A detected change remains latched until sdcard_init() succeeds again.
 */
sdcard_result_t sdcard_check_media(sdcard_media_event_t *event);

/**
 * @brief Return the current software-detected media state.
 * @return Current media state; no card communication is performed.
 */
sdcard_media_state_t sdcard_get_media_state(void);

/**
 * @brief Return the monotonically increasing media validity generation.
 * @return Current generation; zero before the first successful initialization.
 */
uint32_t sdcard_get_media_generation(void);

/**
 * @brief Return the driver's cached initialization state.
 * @return true after successful initialization, false otherwise.
 * @note This function does not communicate with the card.
 */
bool sdcard_is_ready(void);

/**
 * @brief Return the card type detected by the latest initialization attempt.
 * @return Detected card type, or SDCARD_TYPE_UNKNOWN.
 */
sdcard_type_t sdcard_get_type(void);

/**
 * @brief Read one or more consecutive logical sectors.
 * @param lba First logical sector to read.
 * @param buffer Destination buffer of at least @p count * SDCARD_BLOCK_SIZE bytes.
 * @param count Number of sectors to read; must be greater than zero.
 * @return SDCARD_OK on success; otherwise an error code.
 * @note When the async transport selects DMA on this core, this call pumps a
 *       private request to completion; otherwise it uses the CPU/SPI backend.
 */
sdcard_result_t sdcard_read_blocks(uint32_t lba, uint8_t *buffer, size_t count);

/**
 * @brief Write one or more consecutive logical sectors.
 * @param lba First logical sector to write.
 * @param buffer Source buffer containing @p count * SDCARD_BLOCK_SIZE bytes.
 * @param count Number of sectors to write; must be greater than zero.
 * @return SDCARD_OK when the data was accepted; otherwise an error code.
 * @note Call sdcard_sync() before assuming internal card programming is complete.
 * @note When the async transport selects DMA on this core, this call pumps a
 *       private request to completion; otherwise it uses the CPU/SPI backend.
 */
sdcard_result_t sdcard_write_blocks(uint32_t lba, const uint8_t *buffer, size_t count);

/**
 * @brief Wait until the card reports ready after a write or erase operation.
 * @return SDCARD_OK if the card became ready; otherwise an error code.
 */
sdcard_result_t sdcard_sync(void);

/**
 * @brief Read the two-byte SPI card status with CMD13.
 * @param status Destination for both response bytes.
 * @return SDCARD_OK if the response was received; otherwise a transport error.
 * @note Card-reported errors are returned in @p status, not as the function result.
 */
sdcard_result_t sdcard_read_status(sdcard_status_t *status);

/**
 * @brief Erase an inclusive range of logical sectors.
 * @param first_lba First sector in the range.
 * @param last_lba Last sector in the range, inclusive.
 * @return SDCARD_OK on success; otherwise an error code.
 * @warning The caller must respect the card's erase granularity.
 */
sdcard_result_t sdcard_erase_blocks(uint32_t first_lba, uint32_t last_lba);

/**
 * @brief Read and decode the CID register.
 * @param cid Destination structure.
 * @return SDCARD_OK on success; otherwise an error code.
 */
sdcard_result_t sdcard_read_cid(sdcard_cid_t *cid);

/**
 * @brief Read and decode the CSD register.
 * @param csd Destination structure.
 * @return SDCARD_OK on success; otherwise an error code.
 */
sdcard_result_t sdcard_read_csd(sdcard_csd_t *csd);

/**
 * @brief Read and decode the OCR register.
 * @param ocr Destination structure.
 * @return SDCARD_OK on success; otherwise an error code.
 */
sdcard_result_t sdcard_read_ocr(sdcard_ocr_t *ocr);

/**
 * @brief Read and decode the SD Configuration Register with ACMD51.
 * @param scr Destination structure.
 * @return SDCARD_OK on success; otherwise an error code.
 */
sdcard_result_t sdcard_read_scr(sdcard_scr_t *scr);

/**
 * @brief Read and decode the 512-bit SD Status with ACMD13.
 * @param status Destination structure.
 * @return SDCARD_OK on success; otherwise an error code.
 */
sdcard_result_t sdcard_read_sd_status(sdcard_sd_status_t *status);

/**
 * @brief Query CMD6 access-mode capabilities without changing card state.
 * @param status Destination for the complete switch-function status.
 * @return SDCARD_OK on success; otherwise an error code.
 */
sdcard_result_t sdcard_get_speed_mode_info(sdcard_switch_status_t *status);

/**
 * @brief Select the default- or high-speed CMD6 access mode.
 * @param mode Requested access mode.
 * @param status Optional destination for the returned switch status; may be NULL.
 * @return SDCARD_OK if the requested mode was selected; otherwise an error code.
 * @note This function does not raise the SPI baud rate automatically.
 */
sdcard_result_t sdcard_set_speed_mode(sdcard_speed_mode_t mode, sdcard_switch_status_t *status);

/**
 * @brief Set the requested SPI baud rate within the active card-mode limit.
 * @param requested_hz Requested baud rate in Hz; must be nonzero.
 * @param actual_hz Optional destination for the baud rate selected by the SDK.
 * @return SDCARD_OK on success; otherwise an error code.
 */
sdcard_result_t sdcard_set_bus_baud_rate(uint32_t requested_hz, uint32_t *actual_hz);

/**
 * @brief Return the current physical SPI baud rate.
 * @return Actual baud rate in Hz, or zero when unavailable.
 */
uint32_t sdcard_get_bus_baud_rate(void);

/**
 * @brief Read the card type, CID, CSD, and OCR.
 * @param info Destination structure, cleared before the first register read.
 * @return SDCARD_OK if every register was read; otherwise the first error.
 * @note Fields read before an error remain available in @p info.
 */
sdcard_result_t sdcard_read_info(sdcard_info_t *info);

/**
 * @brief Query the preferred erase-block size.
 * @param sector_count Receives the erase size in logical sectors.
 * @return SDCARD_OK on success; otherwise an error code.
 */
sdcard_result_t sdcard_get_erase_block_size(uint32_t *sector_count);

/**
 * @brief Configure the async transport on the calling core.
 * @param config Claimed DMA channels, shared IRQ index, and transport mode.
 * @return SDCARD_OK on success; otherwise an error code.
 * @note The driver does not claim or unclaim DMA channels. In IRQ mode it installs
 * a shared handler only for the selected DMA IRQ on the calling core.
 * @note `blocking_transport` affects only block read/write calls. A DMA setting
 *       is used only on this owner core; other cores use the CPU/SPI fallback.
 * @note `allow_task_from_irq` and `allow_callback_from_irq` are independent
 *       opt-ins. The latter requires an ISR-safe user callback.
 * @note Call from the core that will subsequently call sdcard_async_task().
 */
sdcard_result_t sdcard_async_init(const sdcard_async_config_t *config);

/**
 * @brief Return a safe starting configuration for sdcard_async_init().
 * @return A value-initialized configuration with CPU blocking I/O, polling
 *         DMA completion, disabled IRQ-context execution and invalid DMA
 *         channel placeholders (-1).
 * @note Replace both DMA channel placeholders with already-claimed channels
 *       before calling sdcard_async_init().
 */
sdcard_async_config_t sdcard_async_get_default_config(void);

/**
 * @brief Deinitialize an idle async transport without unclaiming DMA channels.
 * @return SDCARD_OK on success; otherwise an error code.
 */
sdcard_result_t sdcard_async_deinit(void);

/**
 * @brief Read transport counters collected since async initialization.
 * @param stats Destination for the counters.
 * @return SDCARD_OK on success; otherwise an error code.
 * @note Polling mode reports zero DMA-IRQ counters. Reading counters does not
 *       change them and is safe while the transport is idle or active.
 * @note With SDCARD_ASYNC_ENABLE_STATS=0, all returned counters are zero.
 */
sdcard_result_t sdcard_async_get_stats(sdcard_async_stats_t *stats);

/**
 * @brief Reset transport counters collected since async initialization.
 * @return SDCARD_OK on success; otherwise an error code.
 * @note The request state is not changed.
 */
sdcard_result_t sdcard_async_reset_stats(void);

/**
 * @brief Queue a block read for cooperative DMA execution.
 * @param lba First logical sector.
 * @param buffer Destination valid through callback dispatch.
 * @param count Number of sectors; must be nonzero.
 * @param timeout_ms Overall timeout or SDCARD_ASYNC_TIMEOUT_INFINITE.
 * @param callback Non-NULL completion callback.
 * @param user_data Opaque callback argument.
 * @return SDCARD_OK when accepted; rejected requests do not generate callbacks.
 */
sdcard_result_t sdcard_read_blocks_async(uint32_t lba, uint8_t *buffer, size_t count, uint32_t timeout_ms, sdcard_async_callback_t callback,
                                         void *user_data);

/**
 * @brief Queue a block read with optional progress notifications.
 * @param lba First logical sector.
 * @param buffer Destination valid through the terminal callback.
 * @param count Number of sectors; must be nonzero.
 * @param timeout_ms Overall timeout or SDCARD_ASYNC_TIMEOUT_INFINITE.
 * @param progress_interval_blocks Progress period; zero disables progress.
 * @param callback Callback receiving PROGRESS and exactly one COMPLETE event.
 * @param user_data Opaque callback argument.
 * @return SDCARD_OK when accepted; rejected requests do not generate callbacks.
 * @note `blocks_transferred` is cumulative. Progress is reported only after a
 *       complete sector has passed the protocol boundary.
 */
sdcard_result_t sdcard_read_blocks_async_ex(uint32_t lba, uint8_t *buffer, size_t count, uint32_t timeout_ms,
                                            size_t progress_interval_blocks, sdcard_async_callback_t callback, void *user_data);

/**
 * @brief Queue a block write for cooperative DMA execution.
 * @param lba First logical sector.
 * @param buffer Source valid through callback dispatch.
 * @param count Number of sectors; must be nonzero.
 * @param timeout_ms Overall timeout or SDCARD_ASYNC_TIMEOUT_INFINITE.
 * @param callback Non-NULL completion callback.
 * @param user_data Opaque callback argument.
 * @return SDCARD_OK when accepted; rejected requests do not generate callbacks.
 * @note Success means the card released its busy state after programming.
 */
sdcard_result_t sdcard_write_blocks_async(uint32_t lba, const uint8_t *buffer, size_t count, uint32_t timeout_ms,
                                          sdcard_async_callback_t callback, void *user_data);

/**
 * @brief Queue a block write with optional progress notifications.
 * @param lba First logical sector.
 * @param buffer Source valid through the terminal callback.
 * @param count Number of sectors; must be nonzero.
 * @param timeout_ms Overall timeout or SDCARD_ASYNC_TIMEOUT_INFINITE.
 * @param progress_interval_blocks Progress period; zero disables progress.
 * @param callback Callback receiving PROGRESS and exactly one COMPLETE event.
 * @param user_data Opaque callback argument.
 * @return SDCARD_OK when accepted; rejected requests do not generate callbacks.
 * @note A write sector is reported only after data acceptance and card busy
 *       completion. Success means the card released its busy state.
 */
sdcard_result_t sdcard_write_blocks_async_ex(uint32_t lba, const uint8_t *buffer, size_t count, uint32_t timeout_ms,
                                             size_t progress_interval_blocks, sdcard_async_callback_t callback, void *user_data);

/**
 * @brief Advance one bounded async state-machine step and dispatch callbacks.
 * @return A scheduling hint for the no-RTOS owner loop.
 * @note Call only from the core that called sdcard_async_init(); do not call
 * concurrently with sdcard_async_deinit(). `WAITING` does not imply that a
 * timer or IRQ will call the task automatically.
 * @note The task is non-reentrant: a nested call records a pending pass and
 * returns SDCARD_ASYNC_TASK_AGAIN without entering the state machine. Direct
 * invocation from alarm/IRQ context is available only through
 * sdcard_async_task_from_irq() when explicitly enabled at initialization.
 */
sdcard_async_task_hint_t sdcard_async_task(void);

/**
 * @brief Advance the async state machine from the owner-core IRQ context.
 * @return A scheduling hint; IRQ_DISABLED means the opt-in is off for the
 *         current phase.
 * @note The caller must be an IRQ handler on the owner core. The task gate is
 *       still non-reentrant. Error recovery, timeout and cancellation cleanup
 *       may be deferred to a later task-context call. An IRQ callback must be
 *       ISR-safe and must not call blocking APIs, allocate, sleep or use a
 *       filesystem.
 */
sdcard_async_task_hint_t sdcard_async_task_from_irq(void);

/**
 * @brief Describe the next async wakeup condition without touching the card.
 * @param schedule Destination for the current request/deadline information.
 * @return SDCARD_OK when async transport is initialized; otherwise an error.
 * @note A finite deadline is absolute and remains the caller's responsibility
 * to schedule. When @p needs_poll is true, the application chooses the poll
 * cadence; no RTOS timer or alarm is installed by the driver.
 */
sdcard_result_t sdcard_async_get_schedule(sdcard_async_schedule_t *schedule);

/**
 * @brief Request cancellation of the active asynchronous operation.
 * @return SDCARD_OK when the request was marked for cancellation;
 *         SDCARD_ERROR_NOT_READY when the transport is not initialized;
 *         SDCARD_ERROR_BUSY when no async request owns the driver.
 * @note Cleanup and the terminal callback occur on a later task call.
 */
sdcard_result_t sdcard_async_cancel(void);

/**
 * @brief Test whether the asynchronous transport owns an active request.
 * @return true while a request is queued, active, aborting or awaiting callback.
 */
bool sdcard_async_busy(void);

/**
 * @brief Return a static human-readable description of a result code.
 * @param result Result code to describe.
 * @return NUL-terminated static string.
 */
const char *sdcard_result_string(sdcard_result_t result);

/**
 * @brief Return a static human-readable description of a card type.
 * @param type Card type to describe.
 * @return NUL-terminated static string.
 */
const char *sdcard_type_string(sdcard_type_t type);

#ifdef __cplusplus
}
#endif

#endif
