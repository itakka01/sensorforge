#pragma once
#include <Arduino.h>
#include <stdint.h>

// Edge acquisition (coordinator HW-time session or passive GPIO meter) for up
// to three externally driven 3.3 V signals. The caller must first establish
// Drone Mode and exclusive ownership of all configured pins (including
// suspending radar/UART and sensor readers).
// This module never enables itself and does not change cluster state.
namespace ClusterHwTimeCapture {
struct Edges {
    int64_t firstRiseUs[3];
    bool received[3];
    uint32_t rises[3];   // every rising edge since begin()/takeAndRearm()
};

// Caller owns the pins for the entire begin/end interval. A false return means
// that no pins or interrupts were acquired. Rejects NONE, duplicate and
// unsuitable GPIO numbers; this is not a replacement for board pinout checks.
bool begin(const int pins[3], bool droneConfirmed, bool exclusivePinsReleased);
void snapshot(Edges &out);
// Atomically returns the current edges and clears them for the next event,
// without detaching interrupts or touching the pin configuration.
void takeAndRearm(Edges &out);
void end();
bool active();
}
