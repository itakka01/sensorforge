# ESP-NOW Zeitstempel-Pfaddiagnose (v87-beta105)

## Zweck
Die Hardwaremessungen zeigten bei drei XIAOs akzeptierte Exchanges, aber Offset-Streuungen von 1500–2384 µs. Dieser Patch misst *zusätzliche* lokale Software-Latenzen, ohne die Offset-Schätzung, HMAC-Protokolle, Capture-Sperren oder Qualitätsgrenzen zu verändern.

## Erfasste Werte im Coordinator-Screen
- **RX-Queue zuletzt/max (µs):** Zeit vom Aufruf `esp_timer_get_time()` am Anfang des ESP-NOW-Receive-Callbacks bis zum Beginn der späteren Bearbeitung durch `service()`. Sie enthält Queue-Wartezeit und Scheduling. Zeitstempel bleibt der Callback-Eintritt, NICHT der Queue-Pop.
- **Reply→Final-Dispatch zuletzt/max (µs):** Zeit zwischen Beginn der softwareseitigen Reply-Sendefunktion und dem Zeitstempel, der im Final-Paket übertragen wird. Einschließlich Paketkodierung/HMAC und Rückkehr von `esp_now_send`. Das sind **keine tatsächlichen RF-Übertragungszeitpunkte**.

Die Maxima gelten seit der letzten ESP-NOW-Initialisierung; sie sind keine Per-Node-Werte. `esp_now_send()` bestätigt lediglich die Annahme des Sendeauftrags und nicht die abgeschlossene Funkaussendung. Höhere Queue-Latenzen sind eine *mögliche* Ursache zusätzlicher Verzögerung; aus ihnen allein folgt kein Offsetfehler, da t2 beim Callback aufgenommen wird.

## Testverfahren
1. Delta auf den aktuellen Stand aufspielen; für diese Coordinator-Diagnose genügt ein Coordinator-Flash.
2. Im Coordinator-Screen die beiden `zuletzt/max`-Werte unter `Coordinator-Funk` ablesen, während XIAOs Probes senden.
3. Mehrmals ablesen, einmal mit möglichst ruhigem WLAN und einmal mit Stream/Last.
4. Die Klartextdiagnose der Nodes mit ihren verworfenen Serien daneben dokumentieren.
5. Keine Genauigkeit aus diesen Software-Zählern ableiten: für die Validierung GPIO-Flanken mit externem Logikanalysator messen.

## Offene Punkte
- Individueller Probenverlauf inklusive `t1,t2,t3,t4` und Funk-TX-Callbacks ist noch nicht erfasst.
- Das Final trägt einen Software-Zeitstempel vor der realen Funkaussendung; deren variable Verzögerung könnte die Offset-Streuung mitverursachen.
- `UTC-Zeit nicht verfügbar` ist ein separater Capture-Zeitbasis-Fehler, nicht Gegenstand dieses Patches.
- Vollständiger Xtensa-Arduino-Build und Hardwaretest nicht durchgeführt.
