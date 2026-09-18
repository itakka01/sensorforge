#include <Arduino.h>
#include <Wire.h>

#define RTC_SDA_PIN 5   // D4
#define RTC_SCL_PIN 6   // D5

#define DS3231_ADDRESS 0x68


uint8_t decToBcd(uint8_t value)
{
    return ((value / 10) << 4) | (value % 10);
}


uint8_t bcdToDec(uint8_t value)
{
    return ((value >> 4) * 10) + (value & 0x0F);
}


bool readRtc(
    uint16_t &year,
    uint8_t &month,
    uint8_t &day,
    uint8_t &hour,
    uint8_t &minute,
    uint8_t &second
)
{
    Wire.beginTransmission(DS3231_ADDRESS);
    Wire.write(0x00);

    if (Wire.endTransmission() != 0)
        return false;

    Wire.requestFrom(DS3231_ADDRESS, 7);

    if (Wire.available() < 7)
        return false;

    second = bcdToDec(Wire.read() & 0x7F);
    minute = bcdToDec(Wire.read() & 0x7F);
    hour   = bcdToDec(Wire.read() & 0x3F);

    Wire.read(); // weekday

    day = bcdToDec(Wire.read() & 0x3F);

    uint8_t monthRegister = Wire.read();
    month = bcdToDec(monthRegister & 0x1F);

    uint8_t yearRegister = Wire.read();
    year = 2000 + bcdToDec(yearRegister);

    return true;
}


bool setRtc(
    uint16_t year,
    uint8_t month,
    uint8_t day,
    uint8_t hour,
    uint8_t minute,
    uint8_t second
)
{
    Wire.beginTransmission(DS3231_ADDRESS);

    Wire.write(0x00);

    Wire.write(decToBcd(second));
    Wire.write(decToBcd(minute));
    Wire.write(decToBcd(hour));

    Wire.write(decToBcd(1));       // weekday, irrelevant here
    Wire.write(decToBcd(day));
    Wire.write(decToBcd(month));
    Wire.write(decToBcd(year - 2000));

    return Wire.endTransmission() == 0;
}


void printRtc()
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;

    if (!readRtc(
            year,
            month,
            day,
            hour,
            minute,
            second
        ))
    {
        Serial.println("RTC read ERROR");
        return;
    }

    Serial.printf(
        "Time on startup: %02u.%02u.%04u %02u:%02u:%02u\n",
        day,
        month,
        year,
        hour,
        minute,
        second
    );
}


void setup()
{
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("=== DS3231 RETENTION TEST ===");

    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;

    if (!readRtc(
            year,
            month,
            day,
            hour,
            minute,
            second
        ))
    {
        Serial.println("DS3231 not found!");
        return;
    }

    // Only set an obviously uninitialized RTC.
    if (year < 2030)
    {
        Serial.println("RTC not initialized - setting test time...");

        if (setRtc(
                2042,
                7,
                15,
                12,
                34,
                0
            ))
        {
            Serial.println("RTC set successfully.");
        }
        else
        {
            Serial.println("RTC set ERROR!");
            return;
        }

        delay(100);
    }
    else
    {
        Serial.println("RTC already initialized - NOT changing time.");
    }

    printRtc();
}


void loop()
{
}