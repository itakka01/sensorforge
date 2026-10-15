# Fix Coordinator-Webseite

Im TX-Callback-Diagnosepatch war eine JavaScript-Zeichenkette in `webconfig_cluster.cpp` falsch abgeschlossen. Dadurch konnte das Status-Rendering der Coordinator-Webseite nicht starten. Dieses Delta ersetzt nur diese Datei. Die serverseitige ESP-NOW-TX-Diagnose bleibt aktiv. Nur Coordinator flashen und Browser hart neu laden. Bei weiterhin langsamem Boot die TX-Instrumentierung separat zurücknehmen.
