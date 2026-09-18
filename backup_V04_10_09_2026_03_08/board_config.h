#pragma once

#include <Arduino.h>

// =============================================================
// BOARD AUSWÄHLEN
// =============================================================

// #define BOARD_FREENOVE
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
#define SD_SPI_FREQUENCY_HZ 4000000UL

#endif
