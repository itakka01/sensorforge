#include "config.h"

int cfg_recording_encryption = 0;
// SRT generation is deliberately disabled in this benchmark. It happens only
// after video recording and would not improve the 4 MHz vs 20 MHz comparison.
int cfg_timestamp_enabled = 0;
