#include "jpeg_mini_encoder.h"

#include <math.h>
#include <string.h>

namespace JpegMini {
namespace {

// Natural index of the i-th coefficient in zigzag order.
constexpr uint8_t kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Annex K quantization tables (natural order).
constexpr uint8_t kLumQuant[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr uint8_t kChromaQuant[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Annex K Huffman tables: code counts per length 1..16, then symbols.
constexpr uint8_t kDcLumBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr uint8_t kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr uint8_t kAcLumBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
constexpr uint8_t kAcLumVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};
constexpr uint8_t kAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
constexpr uint8_t kAcChromaVals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};

constexpr int bitsTotal(const uint8_t (&bits)[16])
{
    int n = 0;
    for (int i = 0; i < 16; ++i)
        n += bits[i];
    return n;
}
static_assert(bitsTotal(kDcLumBits) == 12 && bitsTotal(kDcChromaBits) == 12, "DC table size");
static_assert(bitsTotal(kAcLumBits) == 162 && bitsTotal(kAcChromaBits) == 162, "AC table size");

// Code and length per symbol (Annex C), built at compile time.
struct HuffTable {
    uint16_t code[256];
    uint8_t size[256];
};

template <size_t N>
constexpr HuffTable buildHuff(const uint8_t (&bits)[16], const uint8_t (&vals)[N])
{
    HuffTable t{};
    uint16_t code = 0;
    size_t k = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int i = 0; i < bits[len - 1]; ++i) {
            t.code[vals[k]] = code;
            t.size[vals[k]] = static_cast<uint8_t>(len);
            ++code;
            ++k;
        }
        code = static_cast<uint16_t>(code << 1);
    }
    return t;
}

constexpr HuffTable kDcLum = buildHuff(kDcLumBits, kDcVals);
constexpr HuffTable kDcChroma = buildHuff(kDcChromaBits, kDcVals);
constexpr HuffTable kAcLum = buildHuff(kAcLumBits, kAcLumVals);
constexpr HuffTable kAcChroma = buildHuff(kAcChromaBits, kAcChromaVals);

// DCT basis: kCos[u][x] = C(u)/2 * cos((2x+1)u*pi/16), C(0) = 1/sqrt(2).
struct CosTable {
    float c[8][8];
};
const CosTable &cosTable()
{
    static const CosTable t = [] {
        CosTable r{};
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                r.c[u][x] = (u ? 0.5f : 0.35355339f) * cosf((2.0f * x + 1.0f) * u * 3.14159265f / 16.0f);
        return r;
    }();
    return t;
}

struct Writer {
    uint8_t *out;
    size_t capacity;
    size_t length;
    uint32_t bitBuf;
    int bitCount;
    bool overflow;

    void byte(uint8_t b)
    {
        if (length >= capacity) {
            overflow = true;
            return;
        }
        out[length++] = b;
    }
    void word(uint16_t w)
    {
        byte(static_cast<uint8_t>(w >> 8));
        byte(static_cast<uint8_t>(w));
    }
    void bits(uint32_t value, int count)
    {
        bitBuf = (bitBuf << count) | (value & ((1u << count) - 1u));
        bitCount += count;
        while (bitCount >= 8) {
            const uint8_t b = static_cast<uint8_t>(bitBuf >> (bitCount - 8));
            byte(b);
            if (b == 0xFF)
                byte(0x00);
            bitCount -= 8;
        }
    }
    void flush()
    {
        if (bitCount > 0)
            bits(0x7F, 8 - bitCount);   // pad with 1 bits
    }
};

int bitLength(int v)
{
    if (v < 0)
        v = -v;
    int n = 0;
    while (v) {
        ++n;
        v >>= 1;
    }
    return n;
}

void writeHuffSegment(Writer &w, uint8_t classId, const uint8_t (&bits)[16], const uint8_t *vals, size_t count)
{
    w.word(0xFFC4);
    w.word(static_cast<uint16_t>(2 + 1 + 16 + count));
    w.byte(classId);
    for (uint8_t b : bits)
        w.byte(b);
    for (size_t i = 0; i < count; ++i)
        w.byte(vals[i]);
}

// One 8x8 block: DCT, quantization, Huffman coding. Returns the new DC.
int encodeBlock(Writer &w, const float (&block)[64], const float (&qInv)[64], int prevDc,
                const HuffTable &dc, const HuffTable &ac)
{
    const CosTable &ct = cosTable();
    float tmp[64];
    for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int x = 0; x < 8; ++x)
                s += ct.c[u][x] * block[y * 8 + x];
            tmp[y * 8 + u] = s;
        }
    int coef[64];
    for (int v = 0; v < 8; ++v)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int y = 0; y < 8; ++y)
                s += ct.c[v][y] * tmp[y * 8 + u];
            const int n = v * 8 + u;
            // Baseline AC categories end at 10 bits; DC difference uses up to 11.
            const int c = static_cast<int>(lroundf(s * qInv[n]));
            coef[n] = n ? (c > 1023 ? 1023 : c < -1023 ? -1023 : c) : c;
        }

    const int dcValue = coef[0];
    const int diff = dcValue - prevDc;
    const int dcBits = bitLength(diff);
    w.bits(dc.code[dcBits], dc.size[dcBits]);
    if (dcBits)
        w.bits(static_cast<uint32_t>(diff < 0 ? diff - 1 : diff), dcBits);

    int run = 0;
    for (int i = 1; i < 64; ++i) {
        const int v = coef[kZigzag[i]];
        if (!v) {
            ++run;
            continue;
        }
        while (run > 15) {
            w.bits(ac.code[0xF0], ac.size[0xF0]);
            run -= 16;
        }
        const int n = bitLength(v);
        const uint8_t sym = static_cast<uint8_t>((run << 4) | n);
        w.bits(ac.code[sym], ac.size[sym]);
        w.bits(static_cast<uint32_t>(v < 0 ? v - 1 : v), n);
        run = 0;
    }
    if (run)
        w.bits(ac.code[0x00], ac.size[0x00]);
    return dcValue;
}

void scaledQuant(const uint8_t (&base)[64], int quality, uint8_t (&out)[64])
{
    const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    for (int i = 0; i < 64; ++i) {
        int q = (base[i] * scale + 50) / 100;
        out[i] = static_cast<uint8_t>(q < 1 ? 1 : q > 255 ? 255 : q);
    }
}

} // namespace

size_t encodeRgb565Le(const uint8_t *rgb, uint16_t width, uint16_t height, uint8_t quality,
                      uint8_t *out, size_t capacity)
{
    if (!rgb || !out || !width || !height)
        return 0;
    const int q = quality < 1 ? 1 : quality > 100 ? 100 : quality;
    uint8_t lumQ[64], chromaQ[64];
    scaledQuant(kLumQuant, q, lumQ);
    scaledQuant(kChromaQuant, q, chromaQ);
    // FDCT output carries no extra factor here, so quantize with 1/Q.
    float lumInv[64], chromaInv[64];
    for (int i = 0; i < 64; ++i) {
        lumInv[i] = 1.0f / lumQ[i];
        chromaInv[i] = 1.0f / chromaQ[i];
    }

    Writer w{out, capacity, 0, 0, 0, false};
    w.word(0xFFD8);
    // APP0 JFIF 1.01, no density, no thumbnail.
    static const uint8_t kApp0[] = {0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    for (uint8_t b : kApp0)
        w.byte(b);
    for (int t = 0; t < 2; ++t) {
        w.word(0xFFDB);
        w.word(67);
        w.byte(static_cast<uint8_t>(t));
        const uint8_t *table = t ? chromaQ : lumQ;
        for (int i = 0; i < 64; ++i)
            w.byte(table[kZigzag[i]]);
    }
    // SOF0: 8 bit, 3 components, 1x1 sampling (4:4:4).
    w.word(0xFFC0);
    w.word(17);
    w.byte(8);
    w.word(height);
    w.word(width);
    w.byte(3);
    for (uint8_t c = 1; c <= 3; ++c) {
        w.byte(c);
        w.byte(0x11);
        w.byte(c == 1 ? 0 : 1);
    }
    writeHuffSegment(w, 0x00, kDcLumBits, kDcVals, sizeof(kDcVals));
    writeHuffSegment(w, 0x10, kAcLumBits, kAcLumVals, sizeof(kAcLumVals));
    writeHuffSegment(w, 0x01, kDcChromaBits, kDcVals, sizeof(kDcVals));
    writeHuffSegment(w, 0x11, kAcChromaBits, kAcChromaVals, sizeof(kAcChromaVals));
    // SOS
    static const uint8_t kSos[] = {0xFF, 0xDA, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    for (uint8_t b : kSos)
        w.byte(b);

    int dcY = 0, dcCb = 0, dcCr = 0;
    float by[64], bcb[64], bcr[64];
    for (int my = 0; my < height && !w.overflow; my += 8) {
        for (int mx = 0; mx < width && !w.overflow; mx += 8) {
            for (int y = 0; y < 8; ++y) {
                const int sy = my + y < height ? my + y : height - 1;
                for (int x = 0; x < 8; ++x) {
                    const int sx = mx + x < width ? mx + x : width - 1;
                    const uint8_t *p = rgb + (static_cast<size_t>(sy) * width + sx) * 2;
                    const uint8_t low = p[0], high = p[1];
                    const float r = static_cast<float>((high & 0xF8) | (high >> 5));
                    const float g = static_cast<float>(((high & 0x07) << 5) | ((low & 0xE0) >> 3) | ((high & 0x07) >> 1));
                    const float b = static_cast<float>(((low & 0x1F) << 3) | ((low & 0x1F) >> 2));
                    const int n = y * 8 + x;
                    by[n] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    bcb[n] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    bcr[n] = 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            }
            dcY = encodeBlock(w, by, lumInv, dcY, kDcLum, kAcLum);
            dcCb = encodeBlock(w, bcb, chromaInv, dcCb, kDcChroma, kAcChroma);
            dcCr = encodeBlock(w, bcr, chromaInv, dcCr, kDcChroma, kAcChroma);
        }
    }
    w.flush();
    w.word(0xFFD9);
    return w.overflow ? 0 : w.length;
}

} // namespace JpegMini
