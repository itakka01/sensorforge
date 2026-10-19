#ifndef DAY_ARCHIVE_TOOLS_H
#define DAY_ARCHIVE_TOOLS_H

#include <Arduino.h>
#include <vector>

// Optional EXAPAN archive converter for day ZIP downloads.
// Source: SD:/tools/exapan_archive_convert.cmd (+ README_Exapan_Archive_Converter.md).
// Only released versions (archive_tools_manifest.h, SHA-256) are included.
struct DayArchiveToolFile {
    String name;
    String data;
};

// Must be called before any ZIP bytes are sent. Never fails the download:
// files to append go to 'files'; anything the user should know (missing,
// outdated, modified, unknown version) goes to 'notice' (empty = all fine).
// The verified bytes are returned in RAM (PSRAM for these sizes), so exactly
// the checked content is streamed.
void dayArchiveCollectTools(std::vector<DayArchiveToolFile> &files, String &notice);

static const char DAY_ARCHIVE_TOOLS_NOTICE_NAME[] = "_EXAPAN_KONVERTER_HINWEIS.txt";

#endif
