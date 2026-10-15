#pragma once
#include <Arduino.h>

// Runs one explicitly requested automatic-update preparation cycle.
// Returns a precise status; NEVER changes boot partition or reboots.
// The caller must invoke this from the regular Arduino loop task, not a callback/ISR.
// No periodic schedule is registered by this module.
enum class FirmwareAutoPrepareStatus : uint8_t {
    Disabled,
    NoUpdate,
    Busy,
    CheckFailed,
    StageFailed,
    Staged
};

FirmwareAutoPrepareStatus firmwareAutoPrepareOnce(String &detail);


// Explicit trusted-controller entry point. Not scheduled or registered in UI.
// BootSelected = next boot partition selected, NOT installed/boot-confirmed.
enum class FirmwareAutoActivateStatus : uint8_t {
    Disabled,
    NoUpdate,
    Busy,
    CheckFailed,
    StageFailed,
    ActivationFailed,
    BootSelected
};
FirmwareAutoActivateStatus firmwareAutoStageAndSelectOnce(String &detail);

// Explicit maintenance request, never auto-scheduled. A successful call
// reboots the device and therefore has no "success" return value.
// The stable-boot confirmation is performed independently after reboot.
enum class FirmwareAutoRebootStatus : uint8_t {
    NotStarted,
    RestartReturned
};
FirmwareAutoRebootStatus firmwareAutoStageSelectAndRebootOnce(String &detail);
