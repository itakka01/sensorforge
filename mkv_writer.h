#pragma once

#include <Arduino.h>

void mkvStart(const String &fullpath, int fps);
void mkvAddFrame();

bool mkvEnd();

bool mkvIsOpen();
bool mkvIsHealthy();
bool mkvHitSizeLimit();
uint32_t mkvGetFrameCount();
uint64_t mkvGetBytesWritten();
