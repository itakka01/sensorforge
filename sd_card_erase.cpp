#include "sd_card_erase.h"
#include "board_config.h"

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <string.h>

extern "C" {
#include "ff.h"
#include "diskio.h"
}

#if defined(STORAGE_SPI)
#include <SPI.h>
// Command helpers of the Arduino SD library (sd_diskio.cpp, external linkage).
// They handle CS, CRC and retries exactly like the library's own transfers.
char sdCommand(uint8_t pdrv, char cmd, unsigned int arg, unsigned int *resp);
char sdTransaction(uint8_t pdrv, char cmd, unsigned int arg, unsigned int *resp);
bool sdSelectCard(uint8_t pdrv);
void sdDeselectCard(uint8_t pdrv);
bool sdWait(uint8_t pdrv, int timeout);
#elif defined(STORAGE_SDMMC)
#include <SD_MMC.h>
#include "sdmmc_cmd.h"
#endif

namespace {

const uint32_t SECTORS_PER_MB = 2048UL;
const uint32_t MARKER_COUNT = 64;
#if defined(STORAGE_SPI)
// One ERASE command for the whole card. The card signals busy on MISO until
// it is done; that is polled without blocking the loop. Afterwards the card
// must be initialised again: in SPI mode it ignores further erase commands
// (tested: 256 MB segments, only the first one was cleared, R1/R2 without
// error) and fails reads until re-initialised.
const uint32_t ERASE_SEGMENT_SECTORS = 0xFFFFFFFFUL;
#else
// sdmmc_erase_sectors() waits until the card is ready: keep segments short.
const uint32_t ERASE_SEGMENT_SECTORS = 32UL * SECTORS_PER_MB;
#endif
// Overwrite: the coarse pass writes chunk 0 of every OVERWRITE_STRIDE chunks.
const uint32_t OVERWRITE_STRIDE = 8;

struct EraseState {
    SdCardEraseMethod method = SD_CARD_METHOD_NONE;
    uint8_t pdrv = 0xFF;
    uint32_t sectorCount = 0;
    uint32_t next = 0;           // erase: next sector; overwrite: next chunk index
    uint32_t done = 0;
    uint8_t pass = 0;
    bool stop = false;
    bool busy = false;           // SPI: ERASE running on the card
    bool blockAddressing = true; // SDHC/SDXC address sectors, SDSC bytes
    uint32_t busySinceMs = 0;
    uint32_t busyTimeoutMs = 0;
    uint8_t *buffer = nullptr;
    uint32_t chunkSectors = 0;
};

EraseState g;

uint32_t markerSector(uint32_t index, uint32_t sectorCount)
{
    if (index == 0)
        return 0;
    if (index == MARKER_COUNT - 1)
        return sectorCount - 1;
    // Spread over the card, off the segment/chunk boundaries.
    uint64_t sector =
        (uint64_t)sectorCount * index / MARKER_COUNT +
        (index * 2654435761UL) % 4093UL;
    return sector < sectorCount ? (uint32_t)sector : sectorCount - 1;
}

uint8_t *allocSector()
{
    return (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

#if defined(STORAGE_SPI)

SPISettings spiSettings()
{
    return SPISettings(SD_SPI_NORMAL_FREQUENCY_HZ, MSBFIRST, SPI_MODE0);
}

uint32_t cardAddress(uint32_t sector)
{
    return g.blockAddressing ? sector : sector * 512UL;
}

// CMD32 start, CMD33 end (inclusive), CMD38 erase.
bool spiStartErase(uint32_t first, uint32_t last, String &error)
{
    SPI.beginTransaction(spiSettings());
    char r = (char)0xFF;
    uint8_t failedCmd = 0;
    bool ok = sdSelectCard(g.pdrv);
    if (ok) {
        r = sdCommand(g.pdrv, 32, cardAddress(first), nullptr);
        ok = r == 0;
        failedCmd = 32;
    }
    if (ok) {
        r = sdCommand(g.pdrv, 33, cardAddress(last), nullptr);
        ok = r == 0;
        failedCmd = 33;
    }
    if (ok) {
        r = sdCommand(g.pdrv, 38, 0, nullptr);
        ok = r == 0;
        failedCmd = 38;
    }
    sdDeselectCard(g.pdrv);
    SPI.transfer(0xFF);
    SPI.endTransaction();
    if (!ok) {
        char text[64];
        snprintf(text, sizeof(text), "SD erase CMD%u rejected (R1 0x%02X)",
                 (unsigned)failedCmd, (unsigned)(uint8_t)r);
        error = text;
    }
    return ok;
}

// Card initialisation as in the Arduino SD library (ff_sd_initialize), with
// the library's card entry kept: CMD0, CRC on, CMD8, ACMD41, ACMD42.
bool spiReinitCard(String &error)
{
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(SD_CS_PIN, HIGH);
    for (int i = 0; i < 20; ++i)
        SPI.transfer(0xFF);

    // 0x00 means the card still signals busy: keep trying for up to 10 s.
    char r = (char)0xFF;
    for (int attempt = 0; attempt < 100 && r != 1; ++attempt) {
        digitalWrite(SD_CS_PIN, LOW);
        sdWait(g.pdrv, 500);
        r = sdCommand(g.pdrv, 0, 0, nullptr);
        sdDeselectCard(g.pdrv);
        delay(r == 0 ? 100 : 20);
        esp_task_wdt_reset();
        if (attempt >= 2 && r != 0)
            break;
    }
    bool ok = r == 1;
    uint8_t failedCmd = 0;

    if (ok) {
        r = sdTransaction(g.pdrv, 59, 1, nullptr);
        if (r != 1 && r != 5) {
            delay(10);
            r = sdTransaction(g.pdrv, 59, 1, nullptr);
        }
        ok = r == 1 || r == 5;
        failedCmd = 59;
    }
    uint32_t opCondArg = 0;
    if (ok) {
        unsigned int resp = 0;
        if (sdTransaction(g.pdrv, 8, 0x1AA, &resp) == 1 && (resp & 0xFFF) == 0x1AA)
            opCondArg = 0x40000000UL;
        const uint32_t startMs = millis();
        do {
            r = sdTransaction(g.pdrv, 41, opCondArg, nullptr);
        } while (r == 1 && millis() - startMs < 3000UL);
        ok = r == 0;
        failedCmd = 41;
    }
    if (ok) {
        r = sdTransaction(g.pdrv, 42, 0, nullptr);
        ok = r == 0;
        failedCmd = 42;
    }
    if (ok && !g.blockAddressing) {
        r = sdTransaction(g.pdrv, 16, 512, nullptr);
        ok = r == 0;
        failedCmd = 16;
    }
    SPI.endTransaction();

    if (!ok) {
        char text[64];
        snprintf(text, sizeof(text), "SD re-init after erase failed (CMD%u R1 0x%02X)",
                 (unsigned)failedCmd, (unsigned)(uint8_t)r);
        error = text;
    }
    return ok;
}

// The card holds MISO low while the erase is running; deselecting meanwhile
// is allowed, the erase continues. Ready only when it answers 0xFF on
// several bytes in a row (a single byte right after CS was seen as not busy
// while the card was still erasing).
bool spiCardBusy()
{
    SPI.beginTransaction(spiSettings());
    digitalWrite(SD_CS_PIN, LOW);
    uint8_t ready = 0;
    for (int i = 0; i < 16; ++i)
        ready = SPI.transfer(0xFF) == 0xFF ? ready + 1 : 0;
    digitalWrite(SD_CS_PIN, HIGH);
    SPI.transfer(0xFF);
    SPI.endTransaction();
    return ready < 8;
}

#elif defined(STORAGE_SDMMC)

// SDMMCFS keeps the card handle protected; read it through a derived type.
struct SdMmcCardAccess : fs::SDMMCFS {
    static sdmmc_card_t *card(fs::SDMMCFS &fs)
    {
        return fs.*(&SdMmcCardAccess::_card);
    }
};

#endif

SdCardEraseStepResult eraseStep(uint32_t budgetMs, String &error)
{
    const uint32_t startMs = millis();
    while (true) {
#if defined(STORAGE_SPI)
        if (g.busy) {
            if (!spiCardBusy()) {
                g.busy = false;
                g.done = g.next;
                if (!spiReinitCard(error))
                    return SD_CARD_STEP_FAILED;
            } else if (millis() - g.busySinceMs > g.busyTimeoutMs) {
                error = "SD erase timeout";
                return SD_CARD_STEP_FAILED;
            } else {
                if (millis() - startMs >= budgetMs)
                    return SD_CARD_STEP_RUNNING;
                delay(1);
                continue;
            }
        }
#endif
        if (g.stop || g.next >= g.sectorCount)
            return SD_CARD_STEP_DONE;
        if (millis() - startMs >= budgetMs)
            return SD_CARD_STEP_RUNNING;

        uint32_t first = g.next;
        uint32_t count = g.sectorCount - first;
        if (count > ERASE_SEGMENT_SECTORS)
            count = ERASE_SEGMENT_SECTORS;

#if defined(STORAGE_SPI)
        if (!spiStartErase(first, first + count - 1, error))
            return SD_CARD_STEP_FAILED;
        g.next = first + count;
        g.busy = true;
        g.busySinceMs = millis();
        // Generous: 250 ms per 4 MB allocation unit, at least 10 s.
        g.busyTimeoutMs = (count / (4UL * SECTORS_PER_MB) + 1UL) * 250UL;
        if (g.busyTimeoutMs < 10000UL)
            g.busyTimeoutMs = 10000UL;
#elif defined(STORAGE_SDMMC)
        sdmmc_card_t *card = SdMmcCardAccess::card(SD_MMC);
        if (!card) {
            error = "SD card handle not available";
            return SD_CARD_STEP_FAILED;
        }
        esp_err_t err = sdmmc_erase_sectors(card, first, count, SDMMC_ERASE_ARG);
        if (err != ESP_OK) {
            error = "SD erase failed: " + String(esp_err_to_name(err));
            return SD_CARD_STEP_FAILED;
        }
        g.next = first + count;
        g.done = g.next;
#else
        error = "SD erase not supported";
        return SD_CARD_STEP_FAILED;
#endif
        esp_task_wdt_reset();
    }
}

SdCardEraseStepResult overwriteStep(uint32_t budgetMs, String &error)
{
    const uint32_t startMs = millis();
    const uint32_t chunks =
        (g.sectorCount + g.chunkSectors - 1) / g.chunkSectors;

    while (true) {
        if (g.stop)
            return SD_CARD_STEP_DONE;

        // Pass 1 visits chunks 0, 8, 16, ...; pass 2 all others.
        if (g.pass == 1 && g.next >= chunks) {
            g.pass = 2;
            g.next = 1;
        }
        if (g.pass == 2 && g.next % OVERWRITE_STRIDE == 0)
            g.next++;
        if (g.pass == 2 && g.next >= chunks)
            return SD_CARD_STEP_DONE;
        if (millis() - startMs >= budgetMs)
            return SD_CARD_STEP_RUNNING;

        uint32_t first = g.next * g.chunkSectors;
        uint32_t count = g.sectorCount - first;
        if (count > g.chunkSectors)
            count = g.chunkSectors;

        if (disk_write(g.pdrv, g.buffer, first, count) != RES_OK) {
            error = "SD overwrite failed at sector " + String(first);
            return SD_CARD_STEP_FAILED;
        }
        g.done += count;
        g.next += g.pass == 1 ? OVERWRITE_STRIDE : 1;
        esp_task_wdt_reset();
    }
}

} // namespace

bool sdCardEraseCommandSupported()
{
#if defined(STORAGE_SPI) || defined(STORAGE_SDMMC)
    return true;
#else
    return false;
#endif
}

bool sdCardEraseWriteMarkers(uint8_t pdrv, uint32_t sectorCount, String &error)
{
    uint8_t *sector = allocSector();
    if (!sector) {
        error = "cannot allocate marker buffer";
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; i < MARKER_COUNT && ok; ++i) {
        memset(sector, 0xA5, 512);
        memcpy(sector, "SFERASE1", 8);
        sector[8] = (uint8_t)i;
        ok = disk_write(pdrv, sector, markerSector(i, sectorCount), 1) == RES_OK;
    }
    free(sector);
    if (!ok)
        error = "cannot write erase marker sectors";
    return ok;
}

bool sdCardEraseVerifyMarkers(uint8_t pdrv, uint32_t sectorCount, String &error)
{
    uint8_t *sector = allocSector();
    if (!sector) {
        error = "cannot allocate marker buffer";
        return false;
    }
    uint32_t failed = 0;
    String failedList;
    for (uint32_t i = 0; i < MARKER_COUNT; ++i) {
        uint32_t at = markerSector(i, sectorCount);
        if (disk_read(pdrv, sector, at, 1) != RES_OK) {
            error = "cannot read erase marker sector " + String(at);
            free(sector);
            return false;
        }
        uint8_t value = sector[0];
        bool clear = value == 0x00 || value == 0xFF;
        for (size_t b = 1; clear && b < 512; ++b)
            clear = sector[b] == value;
        if (!clear) {
            if (failed < 4) {
                char item[32];
                snprintf(item, sizeof(item), " #%lu@%lu=%02X",
                         (unsigned long)i, (unsigned long)at, (unsigned)value);
                failedList += item;
            }
            ++failed;
        }
    }
    free(sector);
    if (failed) {
        error = String(failed) + "/" + String(MARKER_COUNT) +
                " erase markers still hold data:" + failedList;
        return false;
    }
    return true;
}

bool sdCardEraseBegin(SdCardEraseMethod method, uint8_t pdrv, uint32_t sectorCount, String &error)
{
    sdCardEraseEnd();
    if (sectorCount == 0) {
        error = "invalid SD sector count";
        return false;
    }
    g.method = method;
    g.pdrv = pdrv;
    g.sectorCount = sectorCount;

    if (method == SD_CARD_METHOD_ERASE) {
        if (!sdCardEraseCommandSupported()) {
            error = "SD erase not supported";
            return false;
        }
        g.blockAddressing = STORAGE.cardType() == CARD_SDHC;
        return true;
    }

    // Overwrite: 32 KB chunks (multi-sector writes), 4 KB if memory is short.
#if defined(STORAGE_SPI)
    const uint32_t caps = MALLOC_CAP_8BIT;
#else
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
#endif
    size_t bytes = 32U * 1024U;
    g.buffer = (uint8_t *)heap_caps_calloc(1, bytes, caps);
    if (!g.buffer) {
        bytes = 4096U;
        g.buffer = (uint8_t *)heap_caps_calloc(1, bytes, caps);
    }
    if (!g.buffer) {
        error = "cannot allocate overwrite buffer";
        g.method = SD_CARD_METHOD_NONE;
        return false;
    }
    g.chunkSectors = bytes / 512U;
    g.pass = 1;
    return true;
}

SdCardEraseStepResult sdCardEraseStep(uint32_t budgetMs, String &error)
{
    if (g.method == SD_CARD_METHOD_ERASE)
        return eraseStep(budgetMs, error);
    if (g.method == SD_CARD_METHOD_OVERWRITE)
        return overwriteStep(budgetMs, error);
    error = "no SD erase job";
    return SD_CARD_STEP_FAILED;
}

void sdCardEraseStop()
{
    g.stop = true;
}

void sdCardEraseEnd()
{
    if (g.buffer)
        free(g.buffer);
    g = EraseState();
}

SdCardEraseMethod sdCardEraseMethod()
{
    return g.method;
}

uint32_t sdCardEraseSectorsDone()
{
    return g.done;
}

uint8_t sdCardErasePass()
{
    return g.pass;
}
