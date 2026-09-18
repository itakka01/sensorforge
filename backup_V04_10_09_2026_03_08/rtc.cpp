#include "rtc.h"

#include "board_config.h"
#include "config.h"

#include <Wire.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

namespace {

static const uint8_t DS3231_ADDRESS = 0x68;
static const uint8_t AT24C32_ADDRESS = 0x57;

static bool detectedState = false;
static bool clockValidState = false;
static bool oscillatorStoppedState = false;
static bool eepromDetectedState = false;
static bool restoredSystemTimeState = false;
static bool rawTimeReadableState = false;

struct RtcFields {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

static RtcFields lastFields = {};


static uint8_t bcdToDec(uint8_t value)
{
    return
        (uint8_t)(
            ((value >> 4) * 10U) +
            (value & 0x0FU)
        );
}


static uint8_t decToBcd(uint8_t value)
{
    return
        (uint8_t)(
            ((value / 10U) << 4) |
            (value % 10U)
        );
}


static bool probeAddress(uint8_t address)
{
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}


static bool readRegister(
    uint8_t reg,
    uint8_t &value
)
{
    Wire.beginTransmission(DS3231_ADDRESS);
    Wire.write(reg);

    if (Wire.endTransmission(false) != 0)
        return false;

    if (Wire.requestFrom(
            DS3231_ADDRESS,
            (uint8_t)1
        ) != 1) {
        return false;
    }

    value = Wire.read();
    return true;
}


static bool writeRegister(
    uint8_t reg,
    uint8_t value
)
{
    Wire.beginTransmission(DS3231_ADDRESS);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}


static bool isLeapYear(uint16_t year)
{
    return
        (year % 4U == 0U) &&
        (
            (year % 100U != 0U) ||
            (year % 400U == 0U)
        );
}


static uint8_t daysInMonth(
    uint16_t year,
    uint8_t month
)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12)
        return 0;

    if (month == 2 && isLeapYear(year))
        return 29;

    return days[month - 1];
}


static bool fieldsStructurallyValid(
    const RtcFields &fields
)
{
    if (
        fields.year < 2000 ||
        fields.year > 2199 ||
        fields.month < 1 ||
        fields.month > 12 ||
        fields.hour > 23 ||
        fields.minute > 59 ||
        fields.second > 59
    ) {
        return false;
    }

    uint8_t maxDay =
        daysInMonth(
            fields.year,
            fields.month
        );

    return
        fields.day >= 1 &&
        fields.day <= maxDay;
}


static bool readFields(RtcFields &fields)
{
    Wire.beginTransmission(DS3231_ADDRESS);
    Wire.write((uint8_t)0x00);

    if (Wire.endTransmission(false) != 0)
        return false;

    if (Wire.requestFrom(
            DS3231_ADDRESS,
            (uint8_t)7
        ) != 7) {
        return false;
    }

    uint8_t secondReg = Wire.read();
    uint8_t minuteReg = Wire.read();
    uint8_t hourReg   = Wire.read();

    Wire.read(); // weekday

    uint8_t dayReg   = Wire.read();
    uint8_t monthReg = Wire.read();
    uint8_t yearReg  = Wire.read();

    fields.second =
        bcdToDec(secondReg & 0x7FU);

    fields.minute =
        bcdToDec(minuteReg & 0x7FU);

    // DS3231 supports 12-hour and 24-hour register formats.
    if (hourReg & 0x40U) {
        uint8_t hour12 =
            bcdToDec(hourReg & 0x1FU);

        bool pm =
            (hourReg & 0x20U) != 0;

        if (hour12 < 1 || hour12 > 12)
            return false;

        fields.hour =
            (uint8_t)(hour12 % 12U);

        if (pm)
            fields.hour += 12U;

    } else {
        fields.hour =
            bcdToDec(hourReg & 0x3FU);
    }

    fields.day =
        bcdToDec(dayReg & 0x3FU);

    fields.month =
        bcdToDec(monthReg & 0x1FU);

    fields.year =
        (uint16_t)(
            2000U +
            bcdToDec(yearReg)
        );

    if (monthReg & 0x80U)
        fields.year += 100U;

    return fieldsStructurallyValid(fields);
}


// Howard Hinnant's civil-date conversion, adapted for Unix epoch days.
// This avoids changing the process timezone just to interpret the RTC,
// because SensorForge stores UTC in the DS3231.
static int64_t daysFromCivil(
    int year,
    unsigned month,
    unsigned day
)
{
    year -=
        month <= 2;

    const int era =
        (year >= 0 ? year : year - 399) /
        400;

    const unsigned yoe =
        (unsigned)(year - era * 400);

    const unsigned shiftedMonth =
        (unsigned)(
            (int)month +
            (month > 2 ? -3 : 9)
        );

    const unsigned doy =
        (
            153U *
            shiftedMonth +
            2U
        ) /
        5U +
        day -
        1U;

    const unsigned doe =
        yoe * 365U +
        yoe / 4U -
        yoe / 100U +
        doy;

    return
        (int64_t)era * 146097LL +
        (int64_t)doe -
        719468LL;
}


static bool fieldsToEpochUtc(
    const RtcFields &fields,
    time_t &epoch
)
{
    if (!fieldsStructurallyValid(fields))
        return false;

    int64_t days =
        daysFromCivil(
            fields.year,
            fields.month,
            fields.day
        );

    int64_t seconds =
        days * 86400LL +
        (int64_t)fields.hour * 3600LL +
        (int64_t)fields.minute * 60LL +
        (int64_t)fields.second;

    if (seconds < 0)
        return false;

    epoch =
        (time_t)seconds;

    return true;
}


static int firmwareBuildYear()
{
    // __DATE__ format is always "Mmm dd yyyy".
    const char *date = __DATE__;

    if (!date || strlen(date) < 11)
        return 2021;

    int year =
        atoi(date + 7);

    if (year < 2021 || year > 2199)
        return 2021;

    return year;
}


static bool fieldsPlausibleForFirmware(
    const RtcFields &fields
)
{
    if (!fieldsStructurallyValid(fields))
        return false;

    int buildYear =
        firmwareBuildYear();

    // Reject obviously stale/corrupt/test dates. One year before the build
    // tolerates year-boundary builds; ten years ahead still allows long-lived
    // installations while catching grossly wrong RTC contents.
    return
        fields.year >= (uint16_t)(buildYear - 1) &&
        fields.year <= (uint16_t)(buildYear + 10);
}


static void applyConfiguredTimezone()
{
    if (!cfg_timezone.length())
        return;

    setenv(
        "TZ",
        cfg_timezone.c_str(),
        1
    );

    tzset();
}


static bool setSystemTimeFromRtc(
    const RtcFields &fields
)
{
    time_t epoch;

    if (!fieldsToEpochUtc(
            fields,
            epoch
        )) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;

    return
        settimeofday(
            &tv,
            nullptr
        ) == 0;
}


static bool writeFields(
    const RtcFields &fields
)
{
    if (!fieldsStructurallyValid(fields))
        return false;

    uint8_t centuryBit =
        fields.year >= 2100
        ? 0x80U
        : 0x00U;

    uint8_t year2 =
        (uint8_t)(fields.year % 100U);

    Wire.beginTransmission(DS3231_ADDRESS);
    Wire.write((uint8_t)0x00);
    Wire.write(decToBcd(fields.second));
    Wire.write(decToBcd(fields.minute));
    Wire.write(decToBcd(fields.hour));
    Wire.write(decToBcd(1)); // weekday is not used by SensorForge
    Wire.write(decToBcd(fields.day));
    Wire.write(
        (uint8_t)(
            decToBcd(fields.month) |
            centuryBit
        )
    );
    Wire.write(decToBcd(year2));

    return Wire.endTransmission() == 0;
}


static String formatLocalEpoch(time_t epoch)
{
    applyConfiguredTimezone();

    struct tm localTime;

    if (!localtime_r(
            &epoch,
            &localTime
        )) {
        return String("invalid");
    }

    char buffer[32];

    if (!strftime(
            buffer,
            sizeof(buffer),
            "%d.%m.%Y %H:%M:%S",
            &localTime
        )) {
        return String("invalid");
    }

    return String(buffer);
}

} // namespace


void rtcBegin()
{
    detectedState = false;
    clockValidState = false;
    oscillatorStoppedState = false;
    eepromDetectedState = false;
    restoredSystemTimeState = false;
    rawTimeReadableState = false;
    lastFields = {};

    // Apply timezone even when no RTC is fitted. This makes an already-valid
    // ESP clock render correctly before any optional NTP transaction occurs.
    applyConfiguredTimezone();

    Wire.begin(
        RTC_SDA_PIN,
        RTC_SCL_PIN
    );

    Wire.setClock(100000UL);

    detectedState =
        probeAddress(DS3231_ADDRESS);

    eepromDetectedState =
        probeAddress(AT24C32_ADDRESS);

    if (!detectedState) {
        Serial.printf(
            "RTC: no supported clock detected | SDA=%d SCL=%d\n",
            RTC_SDA_PIN,
            RTC_SCL_PIN
        );
        return;
    }

    uint8_t status = 0;
    bool statusOk =
        readRegister(
            0x0F,
            status
        );

    oscillatorStoppedState =
        !statusOk ||
        ((status & 0x80U) != 0);

    RtcFields fields;

    rawTimeReadableState =
        readFields(fields);

    if (rawTimeReadableState)
        lastFields = fields;

    bool plausible =
        rawTimeReadableState &&
        fieldsPlausibleForFirmware(fields);

    clockValidState =
        statusOk &&
        !oscillatorStoppedState &&
        plausible;

    if (
        clockValidState &&
        setSystemTimeFromRtc(fields)
    ) {
        restoredSystemTimeState = true;
    } else {
        clockValidState = false;
    }

    Serial.println(
        "RTC: " +
        rtcDiagnosticSummary()
    );
}


bool rtcDetected()
{
    return detectedState;
}


bool rtcClockValid()
{
    return clockValidState;
}


bool rtcOscillatorStopped()
{
    return oscillatorStoppedState;
}


bool rtcEepromDetected()
{
    return eepromDetectedState;
}


bool rtcRestoredSystemTime()
{
    return restoredSystemTimeState;
}


const char *rtcTypeName()
{
    return
        detectedState
        ? "DS3231"
        : "none";
}


uint8_t rtcI2cAddress()
{
    return
        detectedState
        ? DS3231_ADDRESS
        : 0;
}


bool rtcSyncFromSystemTime()
{
    if (!detectedState)
        return false;

    time_t now =
        time(nullptr);

    if (now < (time_t)1609459200)
        return false;

    struct tm utcTime;

    if (!gmtime_r(
            &now,
            &utcTime
        )) {
        return false;
    }

    RtcFields fields;
    fields.year =
        (uint16_t)(utcTime.tm_year + 1900);
    fields.month =
        (uint8_t)(utcTime.tm_mon + 1);
    fields.day =
        (uint8_t)utcTime.tm_mday;
    fields.hour =
        (uint8_t)utcTime.tm_hour;
    fields.minute =
        (uint8_t)utcTime.tm_min;
    fields.second =
        (uint8_t)utcTime.tm_sec;

    if (!writeFields(fields))
        return false;

    uint8_t status = 0;

    if (!readRegister(
            0x0F,
            status
        )) {
        return false;
    }

    status &=
        (uint8_t)~0x80U; // clear OSF only; preserve alarm flags/settings

    if (!writeRegister(
            0x0F,
            status
        )) {
        return false;
    }

    lastFields = fields;
    rawTimeReadableState = true;
    oscillatorStoppedState = false;
    clockValidState = true;

    return true;
}


String rtcTimeText()
{
    if (!detectedState)
        return String("unavailable");

    RtcFields fields;

    if (!readFields(fields))
        return String("invalid");

    lastFields = fields;
    rawTimeReadableState = true;

    time_t epoch;

    if (!fieldsToEpochUtc(
            fields,
            epoch
        )) {
        return String("invalid");
    }

    return formatLocalEpoch(epoch);
}


bool rtcReadTemperatureC(
    float &temperatureC
)
{
    temperatureC = 0.0f;

    if (!detectedState)
        return false;

    // DS3231 temperature registers:
    //   0x11 = signed integer part
    //   0x12 bits 7..6 = fractional quarter degrees
    Wire.beginTransmission(DS3231_ADDRESS);
    Wire.write((uint8_t)0x11);

    if (Wire.endTransmission(false) != 0)
        return false;

    if (Wire.requestFrom(
            DS3231_ADDRESS,
            (uint8_t)2
        ) != 2) {
        return false;
    }

    int8_t whole =
        (int8_t)Wire.read();

    uint8_t fractionReg =
        Wire.read();

    temperatureC =
        (float)whole +
        (float)((fractionReg >> 6) & 0x03U) *
        0.25f;

    return true;
}


String rtcDiagnosticSummary()
{
    if (!detectedState) {
        return
            String("not detected (optional) | SDA=") +
            String((int)RTC_SDA_PIN) +
            " SCL=" +
            String((int)RTC_SCL_PIN);
    }

    String summary =
        String("DS3231 @0x68");

    if (clockValidState) {
        summary +=
            " | clock=valid";

        if (restoredSystemTimeState)
            summary += " | system time restored";

    } else {
        summary +=
            " | clock=invalid";

        if (oscillatorStoppedState)
            summary += " | OSF=set";
        else if (rawTimeReadableState)
            summary += " | date implausible";
        else
            summary += " | read failed";
    }

    summary +=
        eepromDetectedState
        ? " | AT24C32 @0x57"
        : " | EEPROM not detected";

    return summary;
}
