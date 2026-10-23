#pragma once

// WiFi link diagnostics (STA): logs every disconnect with its 802.11 reason
// code, every (re)connect and every IP change on the serial console.
// Diagnostics only: no reconnect logic, no change to WiFi behaviour.
namespace WifiLinkDiag {

// Registers the WiFi event handler once. Safe to call repeatedly.
void begin();

// Prints queued events. Call from loop(); never blocks.
void service();

}  // namespace WifiLinkDiag
