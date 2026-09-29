#pragma once

#include <Arduino.h>

// Appends the Audio / Mikrofon configuration UI only.
// HTTP routes, persistence and audio capture/test logic remain in webconfig.cpp.
void appendWebConfigAudioUi(String &html);
