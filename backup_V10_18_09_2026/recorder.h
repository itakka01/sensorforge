#pragma once

#include <Arduino.h>

// Global recording-start lock.
// Set to true from any module that must temporarily prevent a NEW recording
// from starting. An already running recording is not interrupted.
extern volatile bool g_recordingStartBlocked;

bool recorderStart(const String &fullpath, int fps);
void recorderAddFrame();

// Finalize and rename <final>.part -> <final>.
bool recorderEnd();

bool recorderIsOpen();
bool recorderIsHealthy();
bool recorderHitSizeLimit();
uint32_t recorderGetFrameCount();
uint64_t recorderGetBytesWritten();

String recorderGetFormat();
String recorderGetFinalPath();
