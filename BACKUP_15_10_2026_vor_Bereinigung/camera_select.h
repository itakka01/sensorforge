#pragma once
#include <Arduino.h>
#include <esp_camera.h>
#include "board_config.h"

// -------------------------------------------------------------
// Kamera-Pin-Struktur
// -------------------------------------------------------------
struct camera_pins_t {
    int pin_pwdn;
    int pin_reset;
    int pin_xclk;
    int pin_sscb_sda;
    int pin_sscb_scl;
    int pin_d7;
    int pin_d6;
    int pin_d5;
    int pin_d4;
    int pin_d3;
    int pin_d2;
    int pin_d1;
    int pin_d0;
    int pin_vsync;
    int pin_href;
    int pin_pclk;
};

// -------------------------------------------------------------
// Freenove ESP32-S3 WROOM Kamera-Pins (OV3660)
// -------------------------------------------------------------
#ifdef BOARD_FREENOVE
static const camera_pins_t pins_freenove = {
    .pin_pwdn     = -1,
    .pin_reset    = -1,
    .pin_xclk     = 15,
    .pin_sscb_sda = 4,
    .pin_sscb_scl = 5,
    .pin_d7       = 16,
    .pin_d6       = 17,
    .pin_d5       = 18,
    .pin_d4       = 12,
    .pin_d3       = 10,
    .pin_d2       = 8,
    .pin_d1       = 9,
    .pin_d0       = 11,
    .pin_vsync    = 6,
    .pin_href     = 7,
    .pin_pclk     = 13
};
#endif

// -------------------------------------------------------------
// XIAO ESP32S3 Sense Kamera-Pins (OV2640)
// -------------------------------------------------------------
#ifdef BOARD_XIAO
static const camera_pins_t pins_xiao = {
    .pin_pwdn     = -1,
    .pin_reset    = -1,
    .pin_xclk     = 10,
    .pin_sscb_sda = 40,
    .pin_sscb_scl = 39,

    .pin_d7       = 48,
    .pin_d6       = 11,
    .pin_d5       = 12,
    .pin_d4       = 14,
    .pin_d3       = 16,
    .pin_d2       = 18,
    .pin_d1       = 17,
    .pin_d0       = 15,

    .pin_vsync    = 38,
    .pin_href     = 47,
    .pin_pclk     = 13
};
#endif

// -------------------------------------------------------------
// Kamera-Auswahl nach Modell
// -------------------------------------------------------------
inline camera_pins_t selectCamera(const String &model) {

#ifdef BOARD_FREENOVE
    return pins_freenove;
#endif

#ifdef BOARD_XIAO
    return pins_xiao;
#endif

    // Fallback (falls kein Board definiert)
    camera_pins_t empty = {};
    return empty;
}

// -------------------------------------------------------------
// Auflösung aus config.txt
// -------------------------------------------------------------
inline framesize_t getFrameSize(const String &r) {
    if (r == "160x120") return FRAMESIZE_QQVGA;
    if (r == "320x240") return FRAMESIZE_QVGA;
    if (r == "640x480") return FRAMESIZE_VGA;
    if (r == "800x600") return FRAMESIZE_SVGA;
    if (r == "1024x768") return FRAMESIZE_XGA;
    if (r == "1280x1024") return FRAMESIZE_SXGA;
    if (r == "1600x1200") return FRAMESIZE_UXGA;
    if (r == "2048x1536") return FRAMESIZE_QXGA;

    return FRAMESIZE_XGA; // Default
}

