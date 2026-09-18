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

// Presence input: LD2410S OT2 (HIGH = person present).
// PIR_PIN is retained as a compatibility alias for existing code.
#define PRESENCE_PIN        GPIO_NUM_21
#define PIR_PIN             PRESENCE_PIN
#define MAGNET_SWITCH_PIN   GPIO_NUM_14

// LD2410S UART:
//   sensor OT1/TX -> RADAR_RX_PIN
//   sensor RX     <- RADAR_TX_PIN
#define RADAR_RX_PIN        GPIO_NUM_41
#define RADAR_TX_PIN        GPIO_NUM_42

// RTC / I2C bus.
// GPIO1 and GPIO47 are exposed and unused by camera, SD, radar and magnet input.
#define RTC_SDA_PIN          GPIO_NUM_1
#define RTC_SCL_PIN          GPIO_NUM_47

// User-facing camera rotation is relative to the physically mounted camera.
// This board profile currently needs no additional base rotation.
#define CAMERA_BASE_ROTATION_DEGREES 0

#define STATUS_LED_AVAILABLE 1
#define LED_PIN             GPIO_NUM_2
#define LED_ON_LEVEL        HIGH
#define LED_OFF_LEVEL       LOW

#define SD_MMC_CMD          GPIO_NUM_38
#define SD_MMC_CLK          GPIO_NUM_39
#define SD_MMC_D0           GPIO_NUM_40


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


// Presence input: LD2410S OT2 (HIGH = person present).
// PIR_PIN is retained as a compatibility alias for existing code.
#define PRESENCE_PIN        GPIO_NUM_4      // D3 ← OT2 vom Radar (gelb)
#define PIR_PIN             PRESENCE_PIN
#define MAGNET_SWITCH_PIN   GPIO_NUM_1      // D0 ← Magnetswitch (zweite Leitung Magnetswitch auf GND)

// XIAO hardware UART pins D7/RX and D6/TX.
#define RADAR_RX_PIN GPIO_NUM_44  // D7 ← OT1 vom Radar (weiß)
#define RADAR_TX_PIN GPIO_NUM_43  // D6 → RX am Radar (grau)

// RTC / I2C bus.
#define RTC_SDA_PIN          GPIO_NUM_5      // D4 ← RTC SDA (grün)
#define RTC_SCL_PIN          GPIO_NUM_6      // D5 ← RTC SCL (blau)

// User-facing camera rotation is relative to the physically mounted camera.
// On the current SensorForge XIAO camera assembly the native image orientation
// is 180 degrees from the desired 0-degree display orientation. Keep this
// board/mount correction here instead of changing the generic rotation meaning.
#define CAMERA_BASE_ROTATION_DEGREES 180

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

#if CAMERA_BASE_ROTATION_DEGREES != 0 && CAMERA_BASE_ROTATION_DEGREES != 180
#error "CAMERA_BASE_ROTATION_DEGREES must be 0 or 180"
#endif

#endif