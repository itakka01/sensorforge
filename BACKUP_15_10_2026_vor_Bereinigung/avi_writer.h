#pragma once

#include <Arduino.h>

void aviStart(const String &fullpath, int fps);
void aviAddFrame();

bool aviEnd();

bool aviIsOpen();
bool aviIsHealthy();
bool aviHitSizeLimit();
uint32_t aviGetFrameCount();
uint64_t aviGetBytesWritten();
