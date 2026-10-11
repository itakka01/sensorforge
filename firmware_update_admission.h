#pragma once
#include <Arduino.h>
// Cooperative OTA admission gate. Call exclusively from the ordinary loop task.
// This module does not download, flash or reboot.
namespace FirmwareUpdateAdmission {
  bool tryAcquire(String &reason);
  void release();
  bool active();
}
