#pragma once
// Persistent, independent readiness override. Never overrides thermal emergency
// handling or SD-fault recovery; normal Recording/Shooter settings are preserved.
void droneModeBegin();
bool droneModeEnabled();
bool droneModeSet(bool enabled);
