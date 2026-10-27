#pragma once
#include <Arduino.h>

// Card-level erase for SD Secure Erase (v88-beta190).
//
// Works below FatFs on the physical drive (the FatFs volume must be detached
// with f_mount(nullptr, ...) first). Two methods, both processed in steps from
// the loop task so the WebServer can show progress and accept an abort:
//
// - Erase: the card's own ERASE command (CMD32/33/38) over the whole card in
//   segments. The card controller clears the blocks itself, typically in
//   seconds to minutes. Marker sectors written before and checked afterwards
//   prove that the erase really cleared the data (erased blocks read as all
//   0x00 or all 0xFF).
// - Overwrite (fallback): zero-filled multi-sector writes over the whole card,
//   first a coarse pass (the first chunk of every 8) so an abort leaves every
//   larger file damaged, then the remaining 7/8.
//
// Wear levelling and spare blocks of the card are outside the reach of both.

enum SdCardEraseMethod : uint8_t {
    SD_CARD_METHOD_NONE = 0,
    SD_CARD_METHOD_ERASE,
    SD_CARD_METHOD_OVERWRITE
};

enum SdCardEraseStepResult : uint8_t {
    SD_CARD_STEP_RUNNING = 0,
    SD_CARD_STEP_DONE,      // finished (or stopped after sdCardEraseStop())
    SD_CARD_STEP_FAILED
};

// Card erase command available on this storage backend.
bool sdCardEraseCommandSupported();

// Writes the marker sectors (before an erase).
bool sdCardEraseWriteMarkers(uint8_t pdrv, uint32_t sectorCount, String &error);
// All marker sectors read back as uniform 0x00 or 0xFF.
bool sdCardEraseVerifyMarkers(uint8_t pdrv, uint32_t sectorCount, String &error);

bool sdCardEraseBegin(SdCardEraseMethod method, uint8_t pdrv, uint32_t sectorCount, String &error);
// Works for about budgetMs, then returns.
SdCardEraseStepResult sdCardEraseStep(uint32_t budgetMs, String &error);
// Stop after the segment/chunk in progress; the next step returns DONE.
void sdCardEraseStop();
// Frees the buffers; safe to call at any time.
void sdCardEraseEnd();

SdCardEraseMethod sdCardEraseMethod();
uint32_t sdCardEraseSectorsDone();
// Overwrite pass (1 = coarse, 2 = fill), 0 for erase.
uint8_t sdCardErasePass();
