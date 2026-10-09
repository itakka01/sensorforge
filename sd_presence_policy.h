#pragma once
// Device-local persistent SD expectation. Stored in NVS so it can be read
// before /config.txt and LittleFS have been mounted.
bool sdPresenceExpectNoCard();
bool sdPresenceSetExpectNoCard(bool enabled);
