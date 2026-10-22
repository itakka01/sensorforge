#pragma once
// SFJC1 capture command: optional, explicitly versioned trailing field.
//
//   SFJC1|tag|coordinator|coord_boot|epoch|node|node_boot|seq|utc_us            (9 fields, unchanged)
//   SFJC1|tag|coordinator|coord_boot|epoch|node|node_boot|seq|utc_us|tp1        (10 fields, optional)
//
// "tp1" asks the node to additionally emit its GPIO test pulse inside the real
// capture path. Without the request the coordinator sends the unchanged 9-field
// packet byte for byte. Firmware without this extension rejects 10-field packets
// (strict field count), so a node that cannot honour the request never runs a
// silently different shot; the job then simply receives no acceptance.
// Pure parsing helper without Arduino/ESP-IDF dependencies (host test:
// test_cluster_capture_pulse.cpp.test).
#include <stddef.h>
#include <string.h>

namespace ClusterCaptureOptions {

constexpr size_t kBaseFields = 9;
constexpr const char *kTestPulseToken = "tp1";

// Returns false for every field layout other than the two documented ones.
inline bool parse(size_t fieldCount, const char *const *fields, bool &testPulse) {
    testPulse = false;
    if (fieldCount == kBaseFields) return true;
    if (fieldCount != kBaseFields + 1 || !fields || !fields[kBaseFields]) return false;
    if (strcmp(fields[kBaseFields], kTestPulseToken) != 0) return false;
    testPulse = true;
    return true;
}

}  // namespace ClusterCaptureOptions
