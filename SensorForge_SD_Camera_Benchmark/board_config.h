#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// Minimal XIAO ESP32S3 Sense board profile for the standalone benchmark.
#define BOARD_XIAO
#define STORAGE SD
#define STORAGE_SPI

#define SD_CS_PIN    GPIO_NUM_21
#define SD_SCK_PIN   GPIO_NUM_7
#define SD_MISO_PIN  GPIO_NUM_8
#define SD_MOSI_PIN  GPIO_NUM_9

#define SD_SPI_NORMAL_FREQUENCY_HZ 4000000UL
#define SD_SPI_MAX_FREQUENCY_HZ   20000000UL

#define CAMERA_BASE_ROTATION_DEGREES 180
