# SensorForge v87-beta105 — lokaler Cluster-Status auf Nodes

## Ursache

In `clusterPageHtml()` ruft `render(d)` die Funktion `sfRenderHwTimeInfo()` auf. Diese wird ausschließlich in der Coordinator-Seite `/cluster_coordinate` definiert und ist auf `/cluster` nicht verfügbar. Beim erfolgreichen Empfang von `/cluster_status` (HTTP 200, `ok:true`) führt der unbedingte Aufruf daher zu `ReferenceError`. Der gemeinsame Promise-`catch` gab irreführend `Status nicht erreichbar` aus, obwohl der Netzwerkabruf erfolgreich war.

## Änderung

- Nur in der lokalen `/cluster`-Seite den irrtümlichen Aufruf entfernen.
- Im vorhandenen Fehlerhandler den tatsächlichen JavaScript-Fehler mit `console.error` sichtbar machen und die irreführende reine Netzwerkfehlermeldung ersetzen.
- Keine Änderungen an `/cluster_coordinate`, Cluster-API, Authentifizierung, ESP-NOW, Kamera oder Zeitmodell.

## Flash

Für den gemeldeten Fehler auf `devxiao2` genügt das XIAO-Gerät. Falls auch `devxiao1`/`devxiao3` den Fehler zeigen, denselben integrierten Projektstand auf diese Geräte übertragen. Coordinator braucht für diesen Fix keinen Flash.

## Prüfung

Statische Prüfung: Der ungültige Funktionsaufruf ist in `clusterPageHtml()` entfernt; Coordinator-Aufrufe bleiben erhalten. JavaScript des betroffenen Script-Blocks wird mit Node `--check` überprüft, wenn `node` verfügbar ist. ZIP auf Integrität geprüft. Hardwaretest und vollständiger Firmware-Build sind nicht durchgeführt.
