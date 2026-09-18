#include "logger.h"

void logWrite(const String &msg)
{
    Serial.print("[LOG] ");
    Serial.println(msg);
}

void consoleWrite(const char *tag, const String &msg)
{
    Serial.print("[");
    Serial.print(tag ? tag : "-");
    Serial.print("] ");
    Serial.println(msg);
}
