#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Camera image tuning page (/camera_tuning) with live picture, and the live
// frame endpoint (/camera_frame) used by it and by the coordinator's camera
// overview. Admin only. Works in Drone Mode (the normal preview does not).
void webconfigCameraTuningRegisterRoutes(WebServer &server, String (*htmlHeader)(), String (*htmlFooter)());

// Shared by /camera_tuning_apply and /api/v1/camera/tuning: mode=session
// (apply until reboot), save (persist + apply), reset (firmware defaults,
// saved and applied) or resolution (resolution=WxH: save + camera restart);
// fields aec, ae_level, aec_value, agc, agc_gain, gainceiling, awb, wb_mode
// (or wb_mode=k + wb_kelvin), awb_gain, brightness, contrast, saturation.
// Returns the HTTP status.
int cameraTuningRequestFromArgs(WebServer &srv, String &json);
// Current camera values, saved tuning and the cluster profile state.
String cameraTuningJson();
// Measures the sensor line duration (clusterCaptureMeasureLineTime): {ok,line_us,vts,frame_ms,aec_max}.
String cameraLineTimeJson();
