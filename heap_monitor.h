#pragma once

#include <Arduino.h>

// Periodic heap health monitoring with fragmentation detection.
// Call heapMonitorService() from the main loop or a dedicated task.

// Initialize heap monitoring (call from setup())
void heapMonitorInit();

// Service function - call periodically from loop()
// Logs heap stats every 60 seconds and warns on fragmentation
void heapMonitorService();

// Force immediate heap stats logging (for debugging)
void heapMonitorLogNow();

// One serial line with internal/PSRAM free and largest block at a boot stage.
// No allocation, no SD logging; for comparing boards in the same boot path.
void heapMonitorBootStage(const char *stage);

// Get current heap fragmentation percentage (0-100)
// Returns 100 if largest block is < 10% of total free
uint8_t heapMonitorGetFragmentationPercent();

// Check if heap is critically fragmented
bool heapMonitorIsCritical();
