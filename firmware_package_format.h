#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Signed firmware package (.sfw) format 2 and the update policies that do not
// need crypto (host-testable). Written by sensorforge_firmware_sign.py.
//
// 256-byte header followed by the raw ESP32 application image:
//   0   magic "SFWPKG2\0"
//   8   u8  format (2)
//   9   u8  algorithm (1 = ECDSA P-256 / SHA-256)
//   10  u16 header size (256)
//   12  u32 image size
//   16  [32] SHA-256 of the image
//   48  [32] board id, NUL padded ("xiao", "freenove", "diymore")
//   80  [32] release tag, NUL padded ("v87-beta158")
//   112 [16] reserved (0)
//   --- bytes 0..127 are the signed block: signature = ECDSA over SHA-256(block)
//   128 u16 signature length (DER, <= 72)
//   130 [72] signature
//   202 ... 255 reserved (0)
// Board and release are covered by the signature, so the downgrade and board
// checks below rely only on signed data. Little-endian throughout.
namespace FirmwarePackage {

constexpr size_t kHeaderSize = 256;
constexpr size_t kSignedBlockSize = 128;
constexpr size_t kSignatureMax = 72;
constexpr uint8_t kFormat = 2;
constexpr uint8_t kAlgorithmEcdsaP256Sha256 = 1;
constexpr size_t kFieldLength = 32;

struct Header {
    uint32_t imageSize = 0;
    uint8_t imageDigest[32] = {};
    char board[kFieldLength + 1] = {};
    char release[kFieldLength + 1] = {};
    uint16_t signatureLength = 0;
    uint8_t signature[kSignatureMax] = {};
    uint8_t signedBlock[kSignedBlockSize] = {};
};

inline uint16_t readU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Printable NUL-padded text field; false when not terminated or not printable.
inline bool readText(const uint8_t *p, char out[kFieldLength + 1])
{
    size_t n = 0;
    while (n < kFieldLength && p[n])
        ++n;
    if (n == 0 || n == kFieldLength)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (p[i] < 0x21 || p[i] > 0x7E || p[i] == '|')
            return false;
    for (size_t i = n; i < kFieldLength; ++i)
        if (p[i])
            return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

// Structure only; the signature is verified separately (firmware_security.cpp).
inline bool parseHeader(const uint8_t *h, size_t length, Header &out, const char *&error)
{
    static const uint8_t kMagic[8] = {'S', 'F', 'W', 'P', 'K', 'G', '2', 0};
    out = Header{};
    error = nullptr;
    if (!h || length < kHeaderSize) { error = "firmware package header is incomplete"; return false; }
    if (memcmp(h, kMagic, sizeof(kMagic)) != 0) {
        error = memcmp(h, "SFWPKG1", 7) == 0
            ? "old package format 1 - sign again with the current sensorforge_firmware_sign.py"
            : "not a SensorForge signed firmware package";
        return false;
    }
    if (h[8] != kFormat) { error = "unsupported firmware package version"; return false; }
    if (h[9] != kAlgorithmEcdsaP256Sha256) { error = "unsupported firmware signature algorithm"; return false; }
    if (readU16(h + 10) != kHeaderSize) { error = "invalid firmware package header size"; return false; }
    out.imageSize = readU32(h + 12);
    if (!out.imageSize) { error = "firmware package contains an empty image"; return false; }
    memcpy(out.imageDigest, h + 16, 32);
    if (!readText(h + 48, out.board)) { error = "firmware package board id is invalid"; return false; }
    if (!readText(h + 80, out.release)) { error = "firmware package release tag is invalid"; return false; }
    out.signatureLength = readU16(h + 128);
    if (!out.signatureLength || out.signatureLength > kSignatureMax) {
        error = "firmware package signature length is invalid";
        return false;
    }
    memcpy(out.signature, h + 130, out.signatureLength);
    memcpy(out.signedBlock, h, kSignedBlockSize);
    return true;
}

// Release tags: "vNN-betaMM" or "vNN" (stable, after every beta of NN).
struct Release {
    unsigned long major = 0;
    unsigned long beta = 0;
    bool stable = false;
};

inline bool parseRelease(const char *tag, Release &out)
{
    out = Release{};
    if (!tag || tag[0] != 'v' || tag[1] < '0' || tag[1] > '9')
        return false;
    char *end = nullptr;
    out.major = strtoul(tag + 1, &end, 10);
    if (!end || out.major > 100000UL)
        return false;
    if (*end == '\0') {
        out.stable = true;
        return true;
    }
    if (strncmp(end, "-beta", 5) != 0 || end[5] < '0' || end[5] > '9')
        return false;
    const char *p = end + 5;
    out.beta = strtoul(p, &end, 10);
    return end && *end == '\0' && out.beta <= 100000UL;
}

// <0 a older than b, 0 same, >0 a newer.
inline int compareRelease(const Release &a, const Release &b)
{
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;
    if (a.stable != b.stable)
        return a.stable ? 1 : -1;
    if (a.beta != b.beta)
        return a.beta < b.beta ? -1 : 1;
    return 0;
}

// Signed header vs. this device: same board, and not older than the running
// release unless allowOlder. A release that cannot be compared is refused.
inline bool checkPolicy(const Header &h, const char *localBoard, const char *runningRelease, bool allowOlder,
                        const char *&error)
{
    error = nullptr;
    if (!localBoard || strcmp(h.board, localBoard) != 0) {
        error = "firmware package is for another board";
        return false;
    }
    Release pkg, running;
    if (!parseRelease(h.release, pkg) || !parseRelease(runningRelease, running)) {
        error = "firmware release tag cannot be compared";
        return false;
    }
    if (!allowOlder && compareRelease(pkg, running) < 0) {
        error = "firmware package is older than the installed firmware (downgrade not allowed)";
        return false;
    }
    return true;
}

// Download sources: https:// to any host, or plain http:// only to a private
// IPv4 address (10/8, 172.16/12, 192.168/16) with optional port. Plain HTTP is
// acceptable there because the package signature, not the transport,
// authenticates the firmware. No credentials, fragments or control chars.
inline bool urlAllowed(const char *url, const char *&error)
{
    error = nullptr;
    if (!url) { error = "URL fehlt"; return false; }
    const size_t len = strlen(url);
    if (len < 10 || len > 240) { error = "URL-Länge ungültig"; return false; }
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)url[i];
        if (c <= 32 || c >= 127 || c == '@' || c == '#' || c == '|' || c == '\\') {
            error = "URL enthält unzulässige Zeichen";
            return false;
        }
    }
    bool https = false;
    const char *host = nullptr;
    if (strncmp(url, "https://", 8) == 0) { https = true; host = url + 8; }
    else if (strncmp(url, "http://", 7) == 0) host = url + 7;
    else { error = "nur http:// oder https://"; return false; }
    const char *hostEnd = host;
    while (*hostEnd && *hostEnd != '/' && *hostEnd != ':' && *hostEnd != '?')
        ++hostEnd;
    if (hostEnd == host) { error = "Host fehlt"; return false; }
    if (*hostEnd == ':') {
        const char *p = hostEnd + 1;
        unsigned long port = 0;
        size_t digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 6) { port = port * 10 + (unsigned long)(*p - '0'); ++p; ++digits; }
        if (!digits || port == 0 || port > 65535 || (*p && *p != '/' && *p != '?')) {
            error = "Port ungültig";
            return false;
        }
    }
    if (https)
        return true;
    unsigned long o[4] = {};
    const char *p = host;
    for (int i = 0; i < 4; ++i) {
        size_t digits = 0;
        unsigned long v = 0;
        while (p < hostEnd && *p >= '0' && *p <= '9' && digits < 4) { v = v * 10 + (unsigned long)(*p - '0'); ++p; ++digits; }
        if (!digits || v > 255) { error = "http:// nur mit privater IPv4-Adresse"; return false; }
        o[i] = v;
        if (i < 3) {
            if (p >= hostEnd || *p != '.') { error = "http:// nur mit privater IPv4-Adresse"; return false; }
            ++p;
        }
    }
    if (p != hostEnd) { error = "http:// nur mit privater IPv4-Adresse"; return false; }
    const bool privateNet = o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168);
    if (!privateNet) { error = "http:// nur mit privater IPv4-Adresse"; return false; }
    return true;
}

} // namespace FirmwarePackage
