#pragma once

#include <Arduino.h>

// =============================================================
// BOARD AUSWÄHLEN
// =============================================================

//#define BOARD_FREENOVE
#define BOARD_XIAO


#if defined(BOARD_FREENOVE) && defined(BOARD_XIAO)
#error "Nur ein Board darf aktiviert sein!"
#endif

#if !defined(BOARD_FREENOVE) && !defined(BOARD_XIAO)
#error "Kein Board ausgewählt!"
#endif


// =============================================================
// FREENOVE FNK0085
// =============================================================

#if defined(BOARD_FREENOVE)

#include <SD_MMC.h>

#define STORAGE SD_MMC
#define STORAGE_SDMMC

// microSD storage policy for this board.
//
// The Freenove FNK0085 uses the ESP32-S3 SD_MMC peripheral rather than the
// SPI-based SD backend used by the XIAO profile below. Therefore this board
// deliberately has NO SD_SPI_NORMAL_FREQUENCY_HZ /
// SD_SPI_MAX_FREQUENCY_HZ definitions. The Sync API high-speed remount logic
// is compiled only for STORAGE_SPI boards and is not applicable here.
//
// If this board is changed to a different storage backend in the future, its
// clock policy belongs here in the board profile, not in config.txt.

// Presence input: LD2410S OT2 or AM312 PIR (HIGH = person present).
// PIR_PIN is retained as a compatibility alias for existing code.
#define PRESENCE_PIN        GPIO_NUM_21      // (gelb)
#define PIR_PIN             PRESENCE_PIN
#define MAGNET_SWITCH_PIN   GPIO_NUM_14     // (braun) andere Seite Magnetswitch auf GND

// LD2410S UART:
//   sensor OT1/TX -> RADAR_RX_PIN
//   sensor RX     <- RADAR_TX_PIN
#define RADAR_RX_PIN        GPIO_NUM_41
#define RADAR_TX_PIN        GPIO_NUM_42

// RTC / I2C bus.
// GPIO1 and GPIO47 are exposed and unused by camera, SD, radar and magnet input.
#define RTC_SDA_PIN          GPIO_NUM_1   // (grün)
#define RTC_SCL_PIN          GPIO_NUM_47  // (blau)

// Fixed board audio hardware.
// External microphones are NOT described here. They are runtime-configurable
// through config.txt / WebConfig Expert mode so one firmware image can support
// different user wiring.
#define BOARD_HAS_INTEGRATED_MIC 0
#define BOARD_INTEGRATED_MIC_NAME "No integrated microphone"

// Board-specific native sensor orientation correction.
// User-facing rotation=0 is defined as the normal, non-mirrored product image.
// Freenove currently needs no additional mirror/flip correction.
#define CAMERA_BASE_HMIRROR 0
#define CAMERA_BASE_VFLIP   0

#define STATUS_LED_AVAILABLE 1
#define LED_PIN             GPIO_NUM_2
#define LED_ON_LEVEL        HIGH
#define LED_OFF_LEVEL       LOW

#define SD_MMC_CMD          GPIO_NUM_38
#define SD_MMC_CLK          GPIO_NUM_39
#define SD_MMC_D0           GPIO_NUM_40

// Recording performance guard.
//
// This is a coarse configuration safety ceiling, not a claimed physical bus
// throughput. The generic config layer computes a weighted pixel rate:
//
//   width * height * fps * JPEG-quality-weight
//
// A value of 0 means that no board-specific ceiling has been qualified yet.
// The Freenove SD_MMC path has not been benchmarked with the same production
// camera/AVI/SFENC1 methodology as the XIAO profile, so do not invent a limit
// here. Once measured, place the validated conservative ceiling here.
#define RECORDING_MAX_WEIGHTED_PIXEL_RATE 0UL


// =============================================================
// XIAO ESP32S3 SENSE
// =============================================================

#elif defined(BOARD_XIAO)

#include <SPI.h>
#include <SD.h>

#define STORAGE SD
#define STORAGE_SPI

// Zuordnungstabelle XIAO ESP32S3 SENSE
// D0 = GPIO1
// D1 = GPIO2
// D2 = GPIO3
// D3 = GPIO4
// D4 = GPIO5
// D5 = GPIO6
// D6 = GPIO43
// D7 = GPIO44


// Presence input: LD2410S OT2 or AM312 PIR (HIGH = person present).
// PIR_PIN is retained as a compatibility alias for existing code.
#define PRESENCE_PIN        GPIO_NUM_4      // D3 ← OT2 vom Radar (gelb)
#define PIR_PIN             PRESENCE_PIN
#define MAGNET_SWITCH_PIN   GPIO_NUM_1      // D0 ← Magnetswitch (zweite Leitung Magnetswitch auf GND) (braun)

// XIAO hardware UART pins D7/RX and D6/TX.
#define RADAR_RX_PIN GPIO_NUM_44  // D7 ← OT1 vom Radar (weiß)
#define RADAR_TX_PIN GPIO_NUM_43  // D6 → RX am Radar (grau)

// RTC / I2C bus.
#define RTC_SDA_PIN          GPIO_NUM_5      // D4 ← RTC SDA (grün)
#define RTC_SCL_PIN          GPIO_NUM_6      // D5 ← RTC SCL (blau)

// Fixed board audio hardware: onboard XIAO ESP32-S3 Sense PDM microphone.
// Seeed hardware routing: GPIO42=PDM CLK, GPIO41=PDM DATA.
// Only physically integrated hardware belongs in the board profile. External
// microphones are selected and wired at runtime through config.txt / WebConfig.
#define BOARD_HAS_INTEGRATED_MIC 1
#define BOARD_INTEGRATED_MIC_BACKEND_PDM 1
#define BOARD_INTEGRATED_MIC_NAME "XIAO onboard PDM microphone"
#define BOARD_INTEGRATED_MIC_PDM_CLK_PIN                  GPIO_NUM_42
#define BOARD_INTEGRATED_MIC_PDM_DATA_PIN                 GPIO_NUM_41
#define BOARD_INTEGRATED_MIC_MIN_SAMPLE_RATE_HZ            16000UL
#define BOARD_INTEGRATED_MIC_MAX_SAMPLE_RATE_HZ            16000UL
#define BOARD_INTEGRATED_MIC_RECOMMENDED_SAMPLE_RATE_HZ    16000UL

// Board-specific native sensor orientation correction.
// On the current SensorForge XIAO + OV3660 assembly the native sensor feed is
// horizontally mirrored relative to the desired product image. Correct that
// mirror here. User-facing rotation=180 is then composed on top by toggling
// both axes, so it remains a true 180-degree rotation without introducing a
// mirror.
#define CAMERA_BASE_HMIRROR 1
#define CAMERA_BASE_VFLIP   0

// This XIAO Sense hardware revision routes microSD CS to GPIO21.
// GPIO21 is also the onboard USER LED, so the firmware must not drive
// that LED independently while the SD card is in use.
#define STATUS_LED_AVAILABLE 0
#define LED_PIN             GPIO_NUM_21
#define LED_ON_LEVEL        LOW
#define LED_OFF_LEVEL       HIGH

#define SD_CS_PIN           GPIO_NUM_21
#define SD_SCK_PIN          GPIO_NUM_7
#define SD_MISO_PIN         GPIO_NUM_8
#define SD_MOSI_PIN         GPIO_NUM_9

// SD-SPI clock policy for this board.
//
// NORMAL is the production clock used for the normal SensorForge workload,
// including recording. A real XIAO ESP32S3 Sense camera/AVI/SFENC1 benchmark
// verified 20 MHz with full read-back integrity and substantially lower frame
// write latency than the former 4 MHz setting. Power measurements showed no
// material increase for the tested workload.
//
// MAX is the board-approved upper production clock used by Sync API diagnostics
// and any future high-speed policy. NORMAL and MAX are intentionally equal on
// this board now, so entering/leaving API-exclusive mode does not require an SD
// remount solely to change the clock. These values are firmware/board knowledge
// and therefore do NOT belong in config.txt.
//
// On the current XIAO ESP32S3 Sense hardware, measurements showed that 20 MHz
// reaches the practical throughput plateau; higher diagnostic test clocks did
// not provide useful additional transfer speed.
#define SD_SPI_NORMAL_FREQUENCY_HZ 20000000UL
#define SD_SPI_MAX_FREQUENCY_HZ    20000000UL

#if SD_SPI_NORMAL_FREQUENCY_HZ == 0 || SD_SPI_MAX_FREQUENCY_HZ == 0
#error "SD SPI frequencies must be greater than zero"
#endif

#if SD_SPI_NORMAL_FREQUENCY_HZ > SD_SPI_MAX_FREQUENCY_HZ
#error "SD_SPI_NORMAL_FREQUENCY_HZ must not exceed SD_SPI_MAX_FREQUENCY_HZ"
#endif

// Recording performance guard for configuration validation.
//
// This is deliberately a conservative UPPER CONFIGURATION CEILING, not an
// exact throughput model and not a promise that every scene/card will sustain
// the limit. The generic config layer computes:
//
//   weighted_pixel_rate = width * height * fps * quality_weight / 100
//
// quality_weight is 100% at JPEG quality=12 and for numerically larger
// (lower-image-quality) values. Numerically smaller JPEG quality values create
// larger JPEGs, so the generic guard adds 5% load per step below 12, capped at
// 160%. Lower JPEG quality must never be used to raise this board ceiling.
//
// Why 8,000,000:
// - real XIAO ESP32S3 Sense + OV3660 + AVI + SFENC1 + SD-SPI 20 MHz benchmark
// - 1024x768 @ 5 fps, JPEG quality 12: VERIFIED, read-back VERIFY=OK
// - P95 frame call about 47 ms, P99 about 49 ms, worst about 54 ms
// - 10 fps gives a 100 ms frame budget and therefore a deliberately useful
//   safety margin over the measured production path
// - 1024x768 @ quality 12 therefore allows max 10 fps (7,864,320 score)
//   while 11 fps is rejected (8,650,752 score)
//
// Re-benchmark before increasing this value, after changing SD backend/clock,
// or when qualifying substantially different camera/board hardware.
#define RECORDING_MAX_WEIGHTED_PIXEL_RATE 8000000UL

#if CAMERA_BASE_HMIRROR != 0 && CAMERA_BASE_HMIRROR != 1
#error "CAMERA_BASE_HMIRROR must be 0 or 1"
#endif

#if CAMERA_BASE_VFLIP != 0 && CAMERA_BASE_VFLIP != 1
#error "CAMERA_BASE_VFLIP must be 0 or 1"
#endif

#endif
