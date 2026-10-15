#include "firmware_ota_verify.h"
#include <stdint.h>

bool firmwareOtaVerifyCompletePackage(
    const SensorForgeFirmwarePackageInfo &package,
    SensorForgeFirmwareHashContext &hash,
    bool boardMarkerFound,
    size_t payloadBytes,
    size_t packageBytes,
    size_t reportedTransportBytes,
    String &error)
{
    // Avoid size_t overflow and truncated packages, even with malformed headers.
    if (package.firmwareSize == 0 ||
        (size_t)package.firmwareSize > SIZE_MAX - SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE) {
        error = "Firmware upload size mismatch - upload may have been interrupted or malformed.";
        return false;
    }
    const size_t expectedBytes =
        SENSORFORGE_FIRMWARE_PACKAGE_HEADER_SIZE + (size_t)package.firmwareSize;
    if (payloadBytes != (size_t)package.firmwareSize ||
        packageBytes != expectedBytes ||
        (reportedTransportBytes != 0 && reportedTransportBytes != packageBytes)) {
        error = "Firmware upload size mismatch - upload may have been interrupted or malformed.";
        return false;
    }
    if (!boardMarkerFound) {
        error = "Firmware rejected: compatibility marker missing or wrong board.";
        return false;
    }
    String signatureError;
    if (!firmwareSecurityHashFinishAndVerify(hash, package, signatureError)) {
        error = "Firmware rejected: " + signatureError + ".";
        return false;
    }
    error = "";
    return true;
}
