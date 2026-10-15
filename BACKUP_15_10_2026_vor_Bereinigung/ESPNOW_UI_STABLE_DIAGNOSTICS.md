# ESP-NOW Diagnose: stabile Coordinator-Anzeige

- Die vorhandenen `<details>`-Elemente werden beim 3-Sekunden-Poll nicht mehr ersetzt. Statusfelder werden gezielt aktualisiert. Der Aufklappzustand und DOM-Fokus bleiben erhalten.
- Zusätzlich gibt es `Diagnose als Klartext anzeigen / ausblenden`, dessen Text unabhängig von Tabellenformatierungen kopiert werden kann.
- Berichtsdaten stammen weiterhin ausschließlich aus der authentifizierten `espnow_node_reports`-Antwort, ohne Schätzer oder Lockerung der Qualitätsfilter.
- Kein Eingriff in Zeitmodell oder Funkprotokoll, daher nur Coordinator neu flashen.
- UTC-Zeitverfügbarkeit ist weiterhin separat zu untersuchen.
- Die zuvor gemeldeten leeren Zellen können auch durch Live-DOM-Ersetzung während des Kopierens entstanden sein; die Änderung eliminiert diesen Mechanismus, beweist aber noch keinen Browser-/Hardwaretest.
