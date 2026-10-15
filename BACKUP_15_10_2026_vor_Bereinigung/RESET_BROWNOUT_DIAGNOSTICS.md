# Reset-/Brownout-Diagnose (SensorForge v87-beta105)

## Umsetzung

`resetDiagnosticsBegin()` wird bei jedem Boot direkt nach `Serial.begin()` aufgerufen. Der ESP32-S3-Resetgrund stammt aus `esp_reset_reason()`; diese API meldet ausschließlich den **letzten Resetgrund**. Normale Power-on-, Software- und Deep-Sleep-Starts werden nicht als Fehler gezählt. Nur `BROWNOUT`, `PANIC`, `INT_WDT`, `TASK_WDT` und `WDT` erhöhen den persistenten NVS-Zähler und fügen einen Eintrag zum Ringpuffer (maximal acht Ereignisse) hinzu. Für diese Ereignisse fallen wenige Flash-Schreibvorgänge an; bei normalen Starts nur Lesezugriffe.

Der allgemeine **Systemstatus**, die ausführlichen **Systeminformationen** und die JSON-API `/ui_status` zeigen Resetgrund und Zähler. Eine Warnung unter der Kopfzeile erscheint nur dann, wenn **der aktuelle Boot** auf einen abnormalen Reset folgt. Alte Brownouts führen nicht zu einer dauerhaft angezeigten Warnung.

**Grenzen:** Der Verlauf enthält Sequenznummern und Resettypen, bewusst keine vermeintlichen UTC-Zeitstempel (UTC könnte beim Boot ungültig sein). Ein Reset, der so früh eintritt, dass NVS nicht mehr geschrieben werden kann, ist nicht garantiert im Verlauf. Die Zähler gelten ab der Installation dieser Funktion und können bei NVS-Löschung verlorengehen. `esp_reset_reason()` misst keine Versorgungsspannung und erkennt nicht jedes kurzzeitige Versorgungstal. Die Weboberfläche liefert keine kontinuierliche Messung einer 5-V-Schiene.

## Test

1. Normaler Kaltstart: `POWERON`, keine Warnung, kein neuer Verlaufseintrag.
2. Einen **bereits beobachteten**, echten Brownout reproduzierbar prüfen; nach Wiederanlauf `BROWNOUT`, Warnbanner und erhöhten Zähler erwarten. Keine künstlichen Kurzschlüsse oder riskanten Stromtests durchführen.
3. Einen normalen Software-Neustart auslösen: `SOFTWARE`, Warnung verschwunden, historische Brownout-Zahl unverändert.
4. `/ui_status` auf `reset_reason`, `reset_abnormal`, `brownout_count`, `abnormal_reset_count` prüfen.
5. Mit Freenove und XIAO jeweils voll kompilieren, UI laden und wiederholte normale Neustarts auf unnötige NVS-Schreibvorgänge prüfen.

Vollständige ESP32-Builds und Hardware-Tests wurden im Delta-Arbeitsgang nicht durchgeführt.
