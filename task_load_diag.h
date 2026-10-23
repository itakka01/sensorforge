#pragma once

// Per-task CPU load on the serial console ([TASK-DIAG]): the busiest tasks
// since the previous report with core and priority. Diagnostics only; it reads
// FreeRTOS run-time statistics and changes nothing.
namespace TaskLoadDiag {

// Call from loop(). Reports every 20 s during the first 2 minutes after boot,
// then every 10 minutes.
void service();

}  // namespace TaskLoadDiag
