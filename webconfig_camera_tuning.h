#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Camera image tuning page (/camera_tuning) with live picture, and the live
// frame endpoint (/camera_frame) used by it and by the coordinator's camera
// overview. Admin only. Works in Drone Mode (the normal preview does not).
void webconfigCameraTuningRegisterRoutes(WebServer &server, String (*htmlHeader)(), String (*htmlFooter)());

// Shared by /camera_tuning_apply and /api/v1/camera/tuning: mode=session
// (apply until reboot), save (persist + apply) or reset (remove the saved
// tuning); fields aec, ae_level, aec_value, agc, agc_gain, gainceiling, awb,
// wb_mode, awb_gain, brightness, contrast, saturation. Returns the HTTP status.
int cameraTuningRequestFromArgs(WebServer &srv, String &json);
// Current camera values, saved tuning and the cluster profile state.
String cameraTuningJson();
