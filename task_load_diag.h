#pragma once

#include <Arduino.h>

// Per-task CPU load on the serial console ([TASK-DIAG]): the busiest tasks
// since the previous report with core and priority. Diagnostics only; it reads
// FreeRTOS run-time statistics and changes nothing.
namespace TaskLoadDiag {

// Call from loop(). Reports every 20 s during the first 2 minutes after boot,
// then every 10 minutes. Independently keeps a 10-second snapshot of the
// busiest tasks for the network (GET /load_diag).
void service();

// JSON object with the busiest tasks of the last 10-second window.
String json();

}  // namespace TaskLoadDiag
