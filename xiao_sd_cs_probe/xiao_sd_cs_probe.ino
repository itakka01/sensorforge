#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

static const int SD_SCK  = 7;
static const int SD_MISO = 8;
static const int SD_MOSI = 9;

static bool trySdCs(int cs)
{
    Serial.printf("\n--- Test SD CS=GPIO%d @ 4 MHz ---\n", cs);

    SD.end();
    SPI.end();
    delay(100);

    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, cs);

    bool mounted = SD.begin(
        cs,
        SPI,
        4000000,
        "/sd",
        5,
        false
    );

    if (!mounted) {
        Serial.println("RESULT: mount FAILED");
        SD.end();
        SPI.end();
        return false;
    }

    uint8_t type = SD.cardType();
    if (type == CARD_NONE) {
        Serial.println("RESULT: mount returned true, but CARD_NONE");
        SD.end();
        SPI.end();
        return false;
    }

    const char *typeName = "UNKNOWN";
    if (type == CARD_MMC) typeName = "MMC";
    else if (type == CARD_SD) typeName = "SDSC";
    else if (type == CARD_SDHC) typeName = "SDHC";

    uint64_t sizeMB = SD.cardSize() / (1024ULL * 1024ULL);

    Serial.printf("RESULT: SUCCESS | type=%s | size=%llu MB\n", typeName, sizeMB);

    File root = SD.open("/");
    if (root) {
        Serial.println("Root directory opened successfully");
        root.close();
    } else {
        Serial.println("WARNING: root directory could not be opened");
    }

    SD.end();
    SPI.end();
    return true;
}

void setup()
{
    Serial.begin(115200);
    delay(2500);

    Serial.println();
    Serial.println("XIAO ESP32S3 Sense SD CS probe");
    Serial.println("SCK=7 MISO=8 MOSI=9");

    bool gpio3 = trySdCs(3);
    delay(500);
    bool gpio21 = trySdCs(21);

    Serial.println();
    Serial.println("=== SUMMARY ===");
    Serial.printf("GPIO3 : %s\n", gpio3 ? "SUCCESS" : "FAILED");
    Serial.printf("GPIO21: %s\n", gpio21 ? "SUCCESS" : "FAILED");
    Serial.println("Test finished.");
}

void loop()
{
    delay(1000);
}
