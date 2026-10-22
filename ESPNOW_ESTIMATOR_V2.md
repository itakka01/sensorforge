# ESP-NOW Schätzer v2 – adaptive Serien und langfristige Drift

Stand: 2026-10-13. Dieser Delta-Patch setzt die letzte `espnow_sync_permission_delta`-Version voraus (einschließlich Capture-Priorität und SyncControl-Freigabe).

## Offset

Der Vier-Zeitstempel-Schätzer validiert kausale Zeitfolgen und begrenzt RTT. Er berechnet drei diagnostische Größen: Offset der Probe mit niedrigster RTT, Median aller gültigen Probes und Median des unteren RTT-Quartils (mindestens drei). **Nur der Median des RTT-Quartils** geht als Offset in das aktuelle Zeitmodell ein. Der minimale RTT-Wert allein garantiert keine symmetrische Funklaufzeit. `spreadUs` bezeichnet ausschließlich die Spannweite des ausgewählten Quartils, nicht eine garantierte Fehlergrenze.

## Adaptive Serien

> **Hinweis (2026-10-23):** Dieser Abschnitt ist historisch. Seit beta131 verwendet der Code fest 16 Austausche je Serie; die Erweiterung auf 32/64 wurde entfernt (siehe `ESPNOW_TIME_ARCHITECTURE.md`, Abschnitt 12.2).

Die Serie startet mit 16 vollständigen Austauschen. Werden die bestehenden Qualitätsgrenzen (mind. acht gültige, Spannweite höchstens 250 µs, Median-RTT höchstens 30 ms) nicht erfüllt, wird sie auf 32, nötigenfalls 64 erweitert. Das Ergebnis wird spätestens bei 64 abgeschlossenen Austauschen bewertet. Zusätzlich bricht der Node eine Serie nach zwölf Sekunden ab, wenn nicht ausreichend Antworten eintreffen. Capture-Pause und entfallene SyncControl-Freigabe verwerfen unvollständige Messungen. Empfangsbereitschaft bleibt erhalten; die letzte gültige Kalibrierung wird nicht gelöscht.

## Drift

Statt einer Einzel-Differenz aus zwei Serien nutzt das Modul bis zu 24 zeitlich geordnete, gültige Messserien. Ein gewichteter Least-Squares-Fit (Gewicht nimmt mit Offset-Spannweite ab) regressiert Offset in µs gegen monotone Node-Zeit in Sekunden. Die Steigung entspricht ppm. Erst ab vier Serien, mindestens drei Minuten Beobachtung, plausibler Steigung (±500 ppm) und begrenzter Residuen-Streuung wird das Modell als gültig markiert. Die vorhandene NVS-Schreibdrossel und die Zuordnung zur stabilen Coordinator-ID bleiben erhalten. Der gespeicherte Drift ist nur eine Startschätzung und ersetzt nach einem Neustart keine aktuelle Offsetmessung.

## Einschränkungen und Testplan

Eine Regression gleicht zufälliges Rauschen aus, aber systematische asymmetrische Software-/WLAN-Verzögerung nicht. Die bestehenden Qualitätsgrenzen sind heuristische Filter, keine gemessenen Genauigkeitsgarantien. Ein vollständiger ESP32-Build und GPIO-/Logikanalysator-Messung stehen aus. Die neue `cluster_espnow_regression.cpp` muss zusammen mit den übrigen Projektdateien in den Arduino-Sketchordner. Der Diagnose-`Estimate` enthält jetzt zusätzlich `minRttOffsetUs`, `allMedianOffsetUs` und `selected`; diese werden nicht automatisch im Web angezeigt.

Vergleiche vor/nach Änderung mindestens 30 Messserien unter identischer Funklast; beobachte erfolgreiche Serien, RTT-Verteilung, Offset-Spannweite, Driftstabilität und die unabhängig gemessene GPIO-Flankenabweichung. Die Capture-Priorität muss separat mit während einer Messserie eintreffenden Aufträgen geprüft werden.
