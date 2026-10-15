# ESP-NOW: Zwei-Wege-Messauswertung (Vorbereitung)

Dieses Delta ergänzt **nur** die vom ESP-NOW-Transport unabhängige Auswertung
für vier Zeitstempel je Messung (t1 Node TX, t2 Coordinator RX,
t3 Coordinator TX, t4 Node RX). Es muss zusätzlich zum vorherigen
ESP-NOW-Funktransport eingespielt werden.

Offset = ((t2-t1)+(t3-t4))/2, positive Werte bedeuten: Coordinator liegt vor Node.
RTT = (t4-t1)-(t3-t2). Für den Offset wird der Median der mindestens drei
Sample mit der kleinsten RTT gebildet. Drift = Veränderung des Offsets relativ
zur verstrichenen Node-Zeit in ppm.

Grenzen: Software-Zeitstempel enthalten Scheduling- und Funkzugriffs-Latenzen;
unsymmetrische Wege verfälschen den Offset. Die angezeigte Sample-Streuung ist
**keine garantierte Zeitgenauigkeit**. Der geschätzte Drift darf nur nach
stabiler Messserie als Kandidat für eine NVS-Kalibrierung gelten.

Noch nicht integriert: authentifizierte Probe/Reply/Final-Funkpakete,
Verknüpfung der vier Stempel über Session-ID/Nonce, Coordinator- und Node-
Zeitmodell, NVS-Übergabe und Webansicht. Die bestehende Cluster-Uhr wird hier
nicht verändert. Keine Firmware-/Hardware-Kompilierung durchgeführt.

Hosttest: `g++ -std=c++17 -Wall -Wextra -Werror ...` bestanden.
