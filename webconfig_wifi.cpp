#include "webconfig_wifi.h"

#include "board_config.h"
#include "config.h"
#include "webconfig_wireguard.h"

#include <WebServer.h>
#include <WiFi.h>
#include <math.h>
#include <stdlib.h>

extern bool wifiInfrastructureRssiAvailable();
extern int wifiInfrastructureRssiDbm();

namespace {

String wifiUiHtmlEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 16);

    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        switch (c) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += c;            break;
        }
    }

    return out;
}

String wifiUiJsonEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 16);

    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        switch (c) {
            case '\\': out += F("\\\\"); break;
            case '"':  out += F("\\\""); break;
            case '\n': out += F("\\n"); break;
            case '\r': out += F("\\r"); break;
            case '\t': out += F("\\t"); break;
            default:
                if ((uint8_t)c >= 0x20U)
                    out += c;
                break;
        }
    }

    return out;
}

} // namespace

String webconfigWifiSettingsHtml(const String &notice)
{
    String html;
    const int wifiTimeoutMinutes =
        cfg_wifi_timeout_sec > 0
        ? (cfg_wifi_timeout_sec + 59) / 60
        : 0;

    WifiAliveScheduleEntry aliveEntries[SENSORFORGE_WIFI_ALIVE_SCHEDULE_MAX_ENTRIES];
    size_t aliveEntryCount = 0;
    String aliveParseError;
    if (!configParseWifiAliveSchedule(
            cfg_wifi_alive_schedule,
            aliveEntries,
            aliveEntryCount,
            &aliveParseError
        )) {
        aliveEntryCount = 0;
    }

    html +=
        "<div class='page-title'><div><h2>WiFi Einstellungen</h2></div></div>";

    html +=
        "<style>"
        ".config-field-grid{display:grid;grid-template-columns:minmax(190px,240px) minmax(220px,1fr);gap:8px 16px;align-items:center}"
        ".config-field-grid .config-label{font-weight:600}"
        ".config-field-grid input,.config-field-grid select{width:100%;max-width:430px;margin:0}"
        ".config-field-grid .config-control{min-width:0}"
        ".config-field-grid .config-note{grid-column:2;color:var(--muted);font-size:.86rem;margin-top:-4px;margin-bottom:4px}"
        ".wifi-network-box{border:1px solid var(--border);border-radius:12px;padding:12px;background:var(--card)}.wifi-profile-list{display:flex;flex-direction:column;gap:8px;margin-top:8px}"
        ".wifi-list-head,.wifi-profile{display:grid;grid-template-columns:80px minmax(180px,1fr) minmax(180px,1fr) auto;gap:10px;align-items:center}.wifi-list-head{font-size:.82rem;font-weight:700;color:var(--muted);padding:0 8px}.wifi-profile{border:1px solid var(--border);border-radius:9px;padding:8px;background:var(--card)}"
        ""
        ".wifi-profile-title{font-weight:700;text-align:center}.wifi-profile-actions{display:flex;gap:6px;flex-wrap:nowrap}"
        ".wifi-profile-actions button{padding:6px 9px;min-width:0}"
        ".wifi-profile input{width:100%;margin:0}.wifi-mode-block[hidden]{display:none!important}"
        ".wifi-scan-row{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-top:12px}.wifi-scan-status{color:var(--muted);font-size:.86rem}.wifi-scan-backdrop{position:fixed;inset:0;background:rgba(15,23,42,.52);display:none;align-items:center;justify-content:center;padding:18px;z-index:10010}.wifi-scan-backdrop.open{display:flex}.wifi-scan-modal{width:min(720px,100%);max-height:82vh;overflow:hidden;background:#fff;border-radius:12px;box-shadow:0 18px 50px rgba(0,0,0,.28);display:flex;flex-direction:column}.wifi-scan-modal-head{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:16px 18px;border-bottom:1px solid var(--border)}.wifi-scan-modal-head h3{margin:0}.wifi-scan-close{font-size:1.35rem;line-height:1;padding:3px 9px;margin:0}.wifi-scan-results{overflow:auto}.wifi-scan-item{display:grid;grid-template-columns:minmax(150px,1fr) 90px 90px auto;gap:10px;align-items:center;padding:10px 18px;border-top:1px solid var(--border)}.wifi-scan-item:first-child{border-top:0}.wifi-scan-item button{padding:6px 10px}"
        ".wifi-info-btn{display:inline-flex;align-items:center;justify-content:center;width:22px;height:22px;min-width:22px;padding:0;margin-left:7px;border:1px solid #9ca3af;border-radius:50%;background:#fff;color:#334155;font-weight:700;line-height:1;cursor:pointer;vertical-align:middle}.wifi-info-btn:hover{background:#f1f5f9}"
        ".wifi-alive-table{display:flex;flex-direction:column;gap:8px;margin-top:10px}.wifi-alive-head,.wifi-alive-row{display:grid;grid-template-columns:minmax(130px,180px) minmax(130px,180px) max-content;gap:10px;align-items:center}.wifi-alive-head{font-size:.82rem;font-weight:700;color:var(--muted);padding:0 8px}.wifi-alive-row{border:1px solid var(--border);border-radius:9px;padding:8px;background:var(--card)}.wifi-alive-row input{width:100%;margin:0}.wifi-alive-actions{display:flex;justify-content:flex-start}.wifi-alive-actions button{padding:6px 10px;margin:0}.wifi-alive-add{margin-top:10px}"
        ".wifi-info-backdrop{position:fixed;inset:0;background:rgba(15,23,42,.48);display:none;align-items:center;justify-content:center;padding:18px;z-index:10000}.wifi-info-backdrop.open{display:flex}.wifi-info-modal{width:min(560px,100%);max-height:80vh;overflow:auto;background:#fff;border-radius:12px;padding:18px;box-shadow:0 18px 50px rgba(0,0,0,.25)}.wifi-info-modal-head{display:flex;align-items:center;justify-content:space-between;gap:12px}.wifi-info-modal-head h3{margin:0}.wifi-info-close{font-size:1.35rem;line-height:1;padding:3px 9px;margin:0}.wifi-info-body{margin-top:12px;line-height:1.45}"
        "@media(max-width:720px){.config-field-grid{grid-template-columns:1fr;gap:4px}.config-field-grid .config-note{grid-column:1;margin-top:-2px;margin-bottom:8px}.config-field-grid input,.config-field-grid select{max-width:none}.wifi-list-head{display:none}.wifi-profile{grid-template-columns:42px 1fr;gap:8px}.wifi-profile>div:nth-of-type(3){grid-column:2}.wifi-profile-actions{grid-column:2;justify-content:flex-end}.wifi-profile-title{text-align:left}.wifi-network-box{padding:9px}.wifi-scan-item{grid-template-columns:1fr auto}.wifi-scan-item .wifi-scan-rssi,.wifi-scan-item .wifi-scan-quality{font-size:.82rem;color:var(--muted)}.wifi-scan-item button{grid-column:2;grid-row:1 / span 2}.wifi-alive-head{display:none}.wifi-alive-row{grid-template-columns:1fr 1fr max-content}.wifi-alive-row input{min-width:0}}"
        "</style>";

    if (notice == "saved") {
        html +=
            "<div class='flash-notice'><strong>WiFi-Einstellungen gespeichert.</strong>"
            "<span class='muted'>Die Zeittabelle gilt sofort; Änderungen am Netzwerk selbst werden nach einem Neustart wirksam.</span></div>";
    }

    html +=
        "<form id='wifiSettingsForm' method='POST' action='/wifi_settings_save'>"
        "<section class='settings-section'>"
        "<div class='config-field-grid'>"
        "<div class='config-label'>Gerätename</div>"
        "<div class='config-control'><input name='hostname' maxlength='63' autocomplete='off' value='" +
        wifiUiHtmlEscape(cfg_hostname) + "'></div>"
        "<div class='config-label'>Verbindungsart</div>"
        "<div class='config-control'><select name='hotspot_enabled' id='wifi-connection-mode'>"
        "<option value='0'" + String(!cfg_hotspot_enabled ? " selected" : "") + ">Externe WLAN-Netzwerke verwenden</option>"
        "<option value='1'" + String(cfg_hotspot_enabled ? " selected" : "") + ">Eigenen Hotspot bereitstellen</option></select></div>"
        "</div></section>";

    html +=
        "<section id='externalWifiBlock' class='settings-section wifi-mode-block'><h3>Externe WLAN-Netzwerke<button type='button' class='wifi-info-btn' aria-label='Information' data-title='Externe WLAN-Netzwerke' data-info='Hier können bis zu 5 bekannte WLAN-Netzwerke gespeichert werden. Die Reihenfolge legt die Verbindungspriorität fest: Eintrag 1 wird zuerst versucht, danach die weiteren Einträge. Mit den Pfeilen lässt sich die Reihenfolge ändern. Über &quot;Verfügbare WLANs anzeigen&quot; können aktuell erreichbare Netzwerke direkt übernommen werden.'>i</button></h3>"
        "<div class='wifi-network-box'>"
        "<div class='wifi-list-head'><span>Priorität</span><span>Netzwerkname (SSID)</span><span>Passwort</span><span>Aktionen</span></div>"
        "<div id='wifiProfileList' class='wifi-profile-list'>";

    size_t configuredProfileCount = 0;
    while (configuredProfileCount < SENSORFORGE_WIFI_PROFILE_COUNT &&
           cfg_wifi_ssids[configuredProfileCount].length() != 0) {
        ++configuredProfileCount;
    }

    for (size_t i = 0; i < SENSORFORGE_WIFI_PROFILE_COUNT; ++i) {
        const bool configured = cfg_wifi_ssids[i].length() != 0;
        const bool visible = configured ||
            (configuredProfileCount < SENSORFORGE_WIFI_PROFILE_COUNT && i == configuredProfileCount);
        html +=
            "<div class='wifi-profile' data-slot='" + String(i + 1) + "'" +
            String(visible ? "" : " style='display:none'") + ">"
            "<div class='wifi-profile-title'>" + String(i + 1) + "</div>"
            "<div><input class='wifi-ssid' name='wifi_ssid_" + String(i + 1) + "' maxlength='32' autocomplete='off' value='" + wifiUiHtmlEscape(cfg_wifi_ssids[i]) + "' placeholder='Netzwerkname (SSID)'></div>"
            "<div><input class='wifi-pass' type='password' name='wifi_pass_" + String(i + 1) + "' maxlength='63' autocomplete='new-password' value='' placeholder='" + String(configured && cfg_wifi_passes[i].length() ? "Gespeichert – leer lassen zum Beibehalten" : "Passwort") + "'></div>"
            "<input class='wifi-source' type='hidden' name='wifi_source_" + String(i + 1) + "' value='" + String(configured ? (int)i + 1 : 0) + "'>"
            "<div class='wifi-profile-actions'><button type='button' class='wifi-up' title='Priorität erhöhen'" + String(configured ? "" : " style='display:none'") + ">↑</button><button type='button' class='wifi-down' title='Priorität verringern'" + String(configured ? "" : " style='display:none'") + ">↓</button><button type='button' class='wifi-delete'" + String(configured ? "" : " style='display:none'") + ">Löschen</button></div>"
            "</div>";
    }

    html +=
        "</div>"
        "<div class='wifi-scan-row'><button id='wifiScanButton' type='button'>Verfügbare WLANs anzeigen</button></div>"
        ""
        "</div>"
        "<div class='config-field-grid' style='margin-top:14px'>"
        "<div class='config-label'>Wenn kein WLAN erreichbar ist</div>"
        "<div class='config-control'><select name='hotspot_fallback_enabled' id='wifi-fallback-mode'>"
        "<option value='0'" + String(!cfg_hotspot_fallback_enabled ? " selected" : "") + ">Offline bleiben</option>"
        "<option value='1'" + String(cfg_hotspot_fallback_enabled ? " selected" : "") + ">Fallback auf eigenen Hotspot</option></select>"
        "<button type='button' class='wifi-info-btn' aria-label='Information' data-title='Fallback auf eigenen Hotspot' data-info='Bei aktiviertem Fallback startet SensorForge den unten konfigurierten Hotspot, wenn keines der gespeicherten WLANs erreichbar ist. Der Fallback-Hotspot verwendet dabei unabhängig von der eingestellten WiFi-Sendeleistung die volle Board-Default-Sendeleistung.'>i</button>"
        "</div>"
        "<div class='config-note'>Hinweis: Der automatische Fallback-Hotspot arbeitet mit voller Board-Default-Sendeleistung (" +
        String(BOARD_WIFI_TX_POWER_DEFAULT_X10 / 10.0f, 1) + " dBm).</div>"
        "</div></section>";

    html +=
        "<section id='hotspotBlock' class='settings-section wifi-mode-block'><h3>Eigener Hotspot</h3>"
        "<div class='config-field-grid'>"
        "<div class='config-label'>Hotspot-Name</div><div class='config-control'><span class='mono'>" + wifiUiHtmlEscape(cfg_hostname) + "</span></div>"
        "<div class='config-note'>Der Hotspot verwendet den oben eingestellten Gerätenamen.</div>"
        "<div class='config-label'>Hotspot-Passwort</div>"
        "<div class='config-control'><input type='password' name='hotspot_password' minlength='8' maxlength='63' autocomplete='new-password' value='' placeholder='Leer lassen = bestehende Einstellung behalten'></div>"
        "<div class='config-note'>Aktuell: " + String(cfg_hotspot_password.length() ? "passwortgeschützt" : "offen / kein Passwort") + ". Neues Passwort: 8 bis 63 Zeichen.</div>"
        "<div class='config-label'>Hotspot-Name sichtbar</div>"
        "<div class='config-control'><select name='hotspot_hidden'>"
        "<option value='0'" + String(!cfg_hotspot_hidden ? " selected" : "") + ">Ja - Netzwerkname wird angezeigt</option>"
        "<option value='1'" + String(cfg_hotspot_hidden ? " selected" : "") + ">Nein - Netzwerkname wird versteckt</option>"
        "</select></div>"
        "</div></section>";

    html +=
        "<section class='settings-section'><h3>Systemzeit</h3>"
        "<div class='config-field-grid'>"
        "<div class='config-label'>Systemzeit beim Start synchronisieren<button type='button' class='wifi-info-btn' aria-label='Information' data-title='Systemzeit synchronisieren' data-info='SensorForge kann beim Start die aktuelle Uhrzeit über ein verbundenes externes WLAN aus dem Internet abrufen und die interne Systemzeit automatisch einstellen.'>i</button></div>"
        "<div class='config-control'><select name='wifi_on_system_start'>"
        "<option value='off'" + String(cfg_wifi_on_system_start == "off" ? " selected" : "") + ">Aus</option>"
        "<option value='on'" + String(cfg_wifi_on_system_start == "on" ? " selected" : "") + ">Immer beim Start synchronisieren</option>"
        "<option value='on_missing_time'" + String(cfg_wifi_on_system_start == "on_missing_time" ? " selected" : "") + ">Nur synchronisieren, wenn noch keine gültige Systemzeit vorhanden ist</option>"
        "</select></div>"
        "<div class='config-label'>WLAN automatisch ausschalten nach<button type='button' class='wifi-info-btn' aria-label='Information' data-title='WLAN automatisch ausschalten' data-info='Diese Einstellung gilt im normalen SensorForge-Betrieb. Nach der eingestellten Zeit ohne WebConfig-Aktivität werden WLAN und WebConfig automatisch ausgeschaltet. 0 Minuten bedeutet: nicht automatisch ausschalten. Im Netzwerk-Streamer wird diese Zeitbegrenzung nicht angewendet, damit RTSP und HTTP dauerhaft erreichbar bleiben.'>i</button></div>"
        "<div class='config-control'><input name='wifi_timeout_minutes' type='number' min='0' max='1440' step='1' value='" +
        String(wifiTimeoutMinutes) + "'></div>"
        "<div class='config-note'>Minuten; 0 = nicht automatisch ausschalten.</div>"
        "</div></section>";

    html +=
        "<section class='settings-section'><h3>Geplante WiFi-Erreichbarkeit"
        "<button type='button' class='wifi-info-btn' aria-label='Information' data-title='Geplante WiFi-Erreichbarkeit' data-info='Die Zeitfenster gelten täglich nach der lokalen SensorForge-Systemzeit. Während eines aktiven Fensters bleibt WiFi unabhängig vom normalen Inaktivitäts-Timeout eingeschaltet. Nach Fensterende wird eine durch den Zeitplan gestartete WiFi-Sitzung erst beendet, wenn kein Web-/API-Zugriff oder API-Exclusive-Download mehr aktiv ist. Magnet-Wakeup und alle bestehenden WiFi-Funktionen bleiben unverändert.'>i</button></h3>"
        "<div class='config-field-grid'>"
        "<div class='config-label'>Zeittabelle verwenden</div>"
        "<div class='config-control'><select name='wifi_alive_schedule_enabled' id='wifiAliveEnabled'>"
        "<option value='0'" + String(!cfg_wifi_alive_schedule_enabled ? " selected" : "") + ">Aus</option>"
        "<option value='1'" + String(cfg_wifi_alive_schedule_enabled ? " selected" : "") + ">Ein</option>"
        "</select></div>"
        "<div class='config-note'>Tägliche lokale Zeitfenster; außerhalb der Fenster gilt die bisherige WiFi-/Magnet-/Timeout-Logik.</div>"
        "</div>"
        "<input type='hidden' name='wifi_alive_schedule' id='wifiAliveSerialized' value='" + wifiUiHtmlEscape(cfg_wifi_alive_schedule) + "'>"
        "<div class='wifi-alive-table' id='wifiAliveTable'>"
        "<div class='wifi-alive-head'><span>Startzeit</span><span>Dauer (Minuten)</span><span>Aktion</span></div>";

    for (size_t i = 0; i < aliveEntryCount; ++i) {
        const unsigned hour = aliveEntries[i].startMinuteOfDay / 60U;
        const unsigned minute = aliveEntries[i].startMinuteOfDay % 60U;
        char timeText[6];
        snprintf(timeText, sizeof(timeText), "%02u:%02u", hour, minute);

        html +=
            "<div class='wifi-alive-row'>"
            "<input class='wifi-alive-time' type='time' step='60' required value='" + String(timeText) + "' aria-label='Startzeit'>"
            "<input class='wifi-alive-duration' type='number' min='1' max='1440' required value='" + String(aliveEntries[i].durationMinutes) + "' aria-label='Dauer in Minuten'>"
            "<div class='wifi-alive-actions'><button type='button' class='wifi-alive-delete'>Löschen</button></div>"
            "</div>";
    }

    html +=
        "</div>"
        "<button type='button' id='wifiAliveAdd' class='wifi-alive-add'>Zeitfenster hinzufügen</button>"
        "<div class='config-note' style='margin-top:8px'>Maximal " + String(SENSORFORGE_WIFI_ALIVE_SCHEDULE_MAX_ENTRIES) + " Zeitfenster. Überlappende Fenster wirken zusammenhängend. Für die Ausführung ist eine gültige Systemzeit erforderlich.</div>";

    if (cfg_wifi_alive_schedule_enabled && !timeIsValid()) {
        html +=
            "<div class='flash-notice' style='margin-top:10px'><strong>Hinweis:</strong> Die Zeittabelle ist aktiviert, aber die Systemzeit ist derzeit nicht gültig. Bis RTC/NTP eine gültige Zeit liefert, werden keine Zeitfenster ausgelöst.</div>";
    }

    html +=
        "</section>";

    html +=
        "<section class='settings-section'><h3>Funkleistung</h3>"
        "<div class='config-field-grid'>"
        "<div class='config-label'>WiFi-Sendeleistung"
        "<button type='button' class='wifi-info-btn' aria-label='Information' data-title='WiFi-Sendeleistung' data-info='Diese Einstellung gilt für die Verbindung mit externen WLANs und für einen bewusst gewählten eigenen Hotspot. Der automatische Fallback-Hotspot verwendet immer die volle Board-Default-Sendeleistung. Der dBm-Wert beschreibt die maximale WiFi-Sendeleistung: Ein höherer Wert bedeutet eine stärkere Funkleistung und kann die Reichweite verbessern. Gleichzeitig können Stromverbrauch und Wärmeentwicklung bei aktiver Funkübertragung steigen.'>i</button>"
        "</div>"
        "<div class='config-control'><select name='wifi_tx_power_dbm'>";

    int16_t configuredTxPowerX10 =
        (int16_t)lroundf(cfg_wifi_tx_power_dbm * 10.0f);

    for (size_t i = 0; i < BOARD_WIFI_TX_POWER_LEVEL_COUNT; ++i) {
        int16_t valueX10 = BOARD_WIFI_TX_POWER_LEVELS_X10[i];
        if (valueX10 < SENSORFORGE_WIFI_TX_POWER_SAFE_MIN_X10)
            continue;
        String valueText = String(valueX10 / 10.0f, 1);
        html +=
            "<option value='" + valueText + "'" +
            String(valueX10 == configuredTxPowerX10 ? " selected" : "") +
            ">" + valueText + " dBm</option>";
    }

    int16_t effectiveTxSafeMinX10 = boardWifiTxPowerSafeMinimumX10();

    html +=
        "</select></div>"
        "<div class='config-note'>Board-Default: <b>" +
        String(BOARD_WIFI_TX_POWER_DEFAULT_X10 / 10.0f, 1) +
        " dBm</b> &nbsp;·&nbsp; Board-Minimum: <b>" +
        String(effectiveTxSafeMinX10 / 10.0f, 1) +
        " dBm</b></div>";

    html +=
        "<div class='config-label'>Empfang externes WLAN</div><div class='config-control'>";

    if (wifiInfrastructureRssiAvailable()) {
        int rssi = wifiInfrastructureRssiDbm();
        int markerPct = constrain((rssi + 90) * 100 / 45, 0, 100);
        String qualityText;
        String recommendation;

        if (rssi >= -55) {
            qualityText = "Sehr gut";
            recommendation = "11 bis 15 dBm ausprobieren";
        } else if (rssi >= -65) {
            qualityText = "Gut";
            recommendation = "15 bis 16.5 dBm ausprobieren";
        } else if (rssi >= -72) {
            qualityText = "Mittel";
            recommendation = "16.5 bis 18 dBm verwenden";
        } else {
            qualityText = "Schwach";
            recommendation = "hohe bzw. maximale Board-Sendeleistung beibehalten";
        }

        html +=
            "<div>Aktuelle Messung: <b>" + String(rssi) + " dBm - " + qualityText + "</b></div>"
            "<div style='position:relative;height:14px;border-radius:7px;margin:8px 0 6px;background:linear-gradient(90deg,#c62828 0%,#f9a825 45%,#7cb342 72%,#2e7d32 100%);'>"
            "<span style='position:absolute;left:calc(" + String(markerPct) + "% - 2px);top:-4px;width:4px;height:22px;background:#111;border-radius:2px'></span></div>"
            "<div class='config-note'>Empfehlung: <b>" + recommendation + "</b>.</div>";
    } else {
        html +=
            "<div class='config-note'>Noch keine Empfangsmessung in dieser Laufzeit. RSSI ist nur bei aktiver Verbindung mit dem externen WLAN messbar.</div>";
    }

    html +=
        "</div></div></section>";

    // VPN belongs to network configuration from the user perspective. Keep
    // the parked WireGuard implementation visible here as a concise product
    // capability state instead of exposing backend/runtime internals.
    html += webconfigWireGuardWifiSectionHtml();

    html +=
        "<div id='wifiScanBackdrop' class='wifi-scan-backdrop' role='dialog' aria-modal='true' aria-hidden='true'>"
        "<div class='wifi-scan-modal'><div class='wifi-scan-modal-head'><h3>Verfügbare WLANs</h3>"
        "<button type='button' id='wifiScanClose' class='wifi-scan-close' aria-label='Schließen'>×</button></div>"
        "<div id='wifiScanStatus' class='wifi-scan-status' style='padding:10px 18px'></div>"
        "<div id='wifiScanResults' class='wifi-scan-results'></div></div></div>"
        "<div id='wifiInfoBackdrop' class='wifi-info-backdrop' role='dialog' aria-modal='true' aria-hidden='true'>"
        "<div class='wifi-info-modal'><div class='wifi-info-modal-head'><h3 id='wifiInfoTitle'>Information</h3>"
        "<button type='button' id='wifiInfoClose' class='wifi-info-close' aria-label='Schließen'>×</button></div>"
        "<div id='wifiInfoBody' class='wifi-info-body'></div></div></div>"
        "<div class='floating-save-space'></div>"
        "<div id='wifiSaveBar' class='floating-save-bar'>"
        "<span id='wifiSaveState' class='floating-save-state'>Keine ungespeicherten Änderungen</span>"
        "<button id='wifiSaveButton' type='submit' disabled>Speichern</button>"
        "</div></form>";

    html +=
        "<script>(function(){"
        "var f=document.getElementById('wifiSettingsForm'),b=document.getElementById('wifiSaveBar'),s=document.getElementById('wifiSaveState'),btn=document.getElementById('wifiSaveButton'),list=document.getElementById('wifiProfileList'),scanBtn=document.getElementById('wifiScanButton'),scanStatus=document.getElementById('wifiScanStatus'),scanResults=document.getElementById('wifiScanResults'),scanBackdrop=document.getElementById('wifiScanBackdrop'),scanClose=document.getElementById('wifiScanClose'),ext=document.getElementById('externalWifiBlock'),ap=document.getElementById('hotspotBlock'),ib=document.getElementById('wifiInfoBackdrop'),it=document.getElementById('wifiInfoTitle'),ic=document.getElementById('wifiInfoBody'),ix=document.getElementById('wifiInfoClose'),aliveTable=document.getElementById('wifiAliveTable'),aliveAdd=document.getElementById('wifiAliveAdd'),aliveSerialized=document.getElementById('wifiAliveSerialized');"
        "if(!f||!b||!s||!btn||!list)return;"
        "function dirty(){b.classList.add('dirty');s.textContent='Ungespeicherte Änderungen';btn.disabled=false;}function updateMode(){var m=f.querySelector(\"select[name='hotspot_enabled']\"),fb=f.querySelector(\"select[name='hotspot_fallback_enabled']\");var hotspot=m&&m.value==='1',fallback=fb&&fb.value==='1';if(ext)ext.hidden=hotspot;if(ap)ap.hidden=!hotspot&&!fallback;}"
        "function rows(){return Array.prototype.slice.call(list.querySelectorAll('.wifi-profile'));}function updateProfileActions(){rows().forEach(function(r){var ssid=r.querySelector('.wifi-ssid'),filled=ssid&&ssid.value.trim();['.wifi-up','.wifi-down','.wifi-delete'].forEach(function(sel){var x=r.querySelector(sel);if(x)x.style.display=filled?'':'none';});});}"
        "function renumber(){rows().forEach(function(r,i){r.querySelector('.wifi-profile-title').textContent=(i+1);r.dataset.slot=i+1;['wifi-ssid','wifi-pass','wifi-source'].forEach(function(c){var e=r.querySelector('.'+c);if(e)e.name=(c==='wifi-ssid'?'wifi_ssid_':c==='wifi-pass'?'wifi_pass_':'wifi_source_')+(i+1);});});}"
        "function ensureBlankRow(){var rs=rows(),visible=rs.filter(function(r){return r.style.display!=='none';}),hasBlank=visible.some(function(r){return !r.querySelector('.wifi-ssid').value.trim();});if(!hasBlank&&visible.length<5){for(var i=0;i<rs.length;i++){if(rs[i].style.display==='none'){rs[i].style.display='';break;}}}updateProfileActions();}"
        "function compactRows(){var rs=rows(),filled=[],empty=[];rs.forEach(function(r){(r.querySelector('.wifi-ssid').value.trim()?filled:empty).push(r);});filled.concat(empty).forEach(function(r){list.appendChild(r);});rs=rows();rs.forEach(function(r,i){r.style.display=(i<filled.length||(filled.length<5&&i===filled.length))?'':'none';});renumber();updateProfileActions();}"
        "function swap(a,c){if(!a||!c)return;var marker=document.createElement('span');a.parentNode.insertBefore(marker,a);c.parentNode.insertBefore(a,c);marker.parentNode.insertBefore(c,marker);marker.remove();renumber();dirty();}"
        "list.addEventListener('click',function(e){var r=e.target.closest('.wifi-profile');if(!r)return;var rs=rows().filter(function(x){return x.style.display!=='none'&&x.querySelector('.wifi-ssid').value.trim();}),i=rs.indexOf(r);if(e.target.classList.contains('wifi-up')&&i>0)swap(r,rs[i-1]);else if(e.target.classList.contains('wifi-down')&&i>=0&&i<rs.length-1)swap(rs[i+1],r);else if(e.target.classList.contains('wifi-delete')){r.querySelector('.wifi-ssid').value='';r.querySelector('.wifi-pass').value='';r.querySelector('.wifi-source').value='0';compactRows();dirty();}});"
        "list.addEventListener('input',function(e){if(e.target.classList.contains('wifi-ssid')){ensureBlankRow();updateProfileActions();}dirty();});"
        "function quality(r){return r>=-55?'Sehr gut':r>=-65?'Gut':r>=-72?'Mittel':'Schwach';}"
        "function configured(ssid){return rows().some(function(r){return r.querySelector('.wifi-ssid').value.trim()===ssid;});}"
        "function chooseNetwork(ssid){var target=rows().find(function(r){return r.style.display!=='none'&&!r.querySelector('.wifi-ssid').value.trim();});if(!target){if(scanStatus)scanStatus.textContent='Maximal 5 WLAN-Profile sind bereits belegt.';return;}target.querySelector('.wifi-ssid').value=ssid;target.querySelector('.wifi-source').value='0';ensureBlankRow();dirty();if(scanBackdrop){scanBackdrop.classList.remove('open');scanBackdrop.setAttribute('aria-hidden','true');}target.querySelector('.wifi-pass').focus();renderScanButtons();}"
        "function renderScanButtons(){if(!scanResults)return;Array.prototype.forEach.call(scanResults.querySelectorAll('[data-ssid]'),function(x){var ssid=x.getAttribute('data-ssid')||'',used=configured(ssid);x.disabled=used;x.textContent=used?'Eingetragen':'Auswählen';});}"
        "if(scanBtn)scanBtn.addEventListener('click',function(){scanBtn.disabled=true;if(scanStatus)scanStatus.textContent='Suche läuft …';if(scanResults)scanResults.innerHTML='';if(scanBackdrop){scanBackdrop.classList.add('open');scanBackdrop.setAttribute('aria-hidden','false');}fetch('/wifi_scan',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('scan');return r.json();}).then(function(data){var nets=(data&&data.networks)||[];if(!scanResults)return;if(!nets.length){scanStatus.textContent='Keine WLAN-Netzwerke gefunden.';return;}scanStatus.textContent=nets.length+' Netzwerk'+(nets.length===1?'':'e')+' gefunden';nets.forEach(function(n){var row=document.createElement('div');row.className='wifi-scan-item';var name=document.createElement('div');name.textContent=n.ssid;var rr=document.createElement('div');rr.className='wifi-scan-rssi';rr.textContent=n.rssi+' dBm';var q=document.createElement('div');q.className='wifi-scan-quality';q.textContent=quality(n.rssi);var pick=document.createElement('button');pick.type='button';pick.setAttribute('data-ssid',n.ssid);pick.addEventListener('click',function(){chooseNetwork(n.ssid);});row.appendChild(name);row.appendChild(rr);row.appendChild(q);row.appendChild(pick);scanResults.appendChild(row);});renderScanButtons();}).catch(function(){if(scanStatus)scanStatus.textContent='WLAN-Suche fehlgeschlagen.';}).then(function(){scanBtn.disabled=false;});});"
        "if(scanBackdrop&&scanClose){function closeScan(){scanBackdrop.classList.remove('open');scanBackdrop.setAttribute('aria-hidden','true');}scanClose.addEventListener('click',closeScan);scanBackdrop.addEventListener('click',function(e){if(e.target===scanBackdrop)closeScan();});document.addEventListener('keydown',function(e){if(e.key==='Escape')closeScan();});}"
        "function aliveRows(){return aliveTable?Array.prototype.slice.call(aliveTable.querySelectorAll('.wifi-alive-row')):[];}"
        "function serializeAlive(){if(!aliveSerialized)return true;var parts=[],seen={},duplicate=false;aliveRows().forEach(function(r){var t=r.querySelector('.wifi-alive-time'),d=r.querySelector('.wifi-alive-duration');var tv=t?t.value.trim():'',dv=d?parseInt(d.value,10):0;if(t)t.setCustomValidity('');if(tv&&dv>=1&&dv<=1440){if(seen[tv]){duplicate=true;if(t)t.setCustomValidity('Diese Startzeit wird bereits von einem anderen Zeitfenster verwendet.');}else{seen[tv]=true;parts.push(tv+'/'+dv);}}});aliveSerialized.value=parts.join(';');return !duplicate;}"
        "function nextAliveTime(){var used={};aliveRows().forEach(function(r){var t=r.querySelector('.wifi-alive-time'),v=t?t.value.trim():'';if(v)used[v]=true;});for(var n=0;n<1440;n+=15){var m=(720+n)%1440,h=Math.floor(m/60),mm=m%60,v=(h<10?'0':'')+h+':'+(mm<10?'0':'')+mm;if(!used[v])return v;}return '12:00';}"
        "function addAliveRow(time,duration){if(!aliveTable||aliveRows().length>=" + String(SENSORFORGE_WIFI_ALIVE_SCHEDULE_MAX_ENTRIES) + ")return;var r=document.createElement('div');r.className='wifi-alive-row';var t=document.createElement('input');t.className='wifi-alive-time';t.type='time';t.step='60';t.required=true;t.value=time||nextAliveTime();t.setAttribute('aria-label','Startzeit');var d=document.createElement('input');d.className='wifi-alive-duration';d.type='number';d.min='1';d.max='1440';d.required=true;d.value=duration||'15';d.setAttribute('aria-label','Dauer in Minuten');var a=document.createElement('div');a.className='wifi-alive-actions';var x=document.createElement('button');x.type='button';x.className='wifi-alive-delete';x.textContent='Löschen';a.appendChild(x);r.appendChild(t);r.appendChild(d);r.appendChild(a);aliveTable.appendChild(r);serializeAlive();dirty();}"
        "if(aliveAdd)aliveAdd.addEventListener('click',function(){addAliveRow(nextAliveTime(),'15');});"
        "if(aliveTable){aliveTable.addEventListener('click',function(e){if(e.target.classList.contains('wifi-alive-delete')){var r=e.target.closest('.wifi-alive-row');if(r){r.remove();serializeAlive();dirty();}}});aliveTable.addEventListener('input',function(){serializeAlive();dirty();});}"
        "f.addEventListener('input',function(e){if(!e.target.classList.contains('wifi-ssid'))dirty();});f.addEventListener('change',function(){dirty();updateMode();});updateMode();ensureBlankRow();"
        "if(ib&&it&&ic&&ix){function closeInfo(){ib.classList.remove('open');ib.setAttribute('aria-hidden','true');}document.querySelectorAll('.wifi-info-btn').forEach(function(x){x.addEventListener('click',function(){it.textContent=x.getAttribute('data-title')||'Information';ic.textContent=x.getAttribute('data-info')||'';ib.classList.add('open');ib.setAttribute('aria-hidden','false');});});ix.addEventListener('click',closeInfo);ib.addEventListener('click',function(e){if(e.target===ib)closeInfo();});document.addEventListener('keydown',function(e){if(e.key==='Escape')closeInfo();});}"
        "f.addEventListener('submit',function(e){renumber();if(!serializeAlive()){e.preventDefault();var bad=aliveTable?aliveTable.querySelector('.wifi-alive-time:invalid'):null;if(bad){bad.reportValidity();bad.focus();}return;}btn.disabled=true;s.textContent='Speichert …';});"
        "})();</script>";

    return html;
}

String webconfigWifiScanJson()
{
    const int found = WiFi.scanNetworks(false, true);

    String json = F("{\"networks\":[");
    if (found > 0) {
        bool first = true;
        for (int i = 0; i < found; ++i) {
            String ssid = WiFi.SSID(i);
            ssid.trim();
            if (!ssid.length())
                continue;

            // Avoid duplicate SSIDs from multiple access points. The scan is
            // RSSI-sorted, so the first occurrence is the strongest one.
            bool duplicate = false;
            for (int j = 0; j < i; ++j) {
                if (WiFi.SSID(j) == ssid) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;

            if (!first)
                json += ',';
            first = false;

            json += F("{\"ssid\":\"");
            json += wifiUiJsonEscape(ssid);
            json += F("\",\"rssi\":");
            json += String(WiFi.RSSI(i));
            json += '}';
        }
    }
    json += F("]}");

    WiFi.scanDelete();
    return json;
}

bool webconfigWifiSaveRequest(
    WebServer &server,
    bool writeToSd,
    String &error
)
{
    String hostname = server.arg("hostname");
    hostname.trim();

    String wifiOnSystemStart = server.arg("wifi_on_system_start");
    wifiOnSystemStart.trim();
    wifiOnSystemStart.toLowerCase();

    int wifiTimeoutSec = 0;
    if (server.hasArg("wifi_timeout_minutes")) {
        const int wifiTimeoutMinutes =
            constrain(server.arg("wifi_timeout_minutes").toInt(), 0, 1440);
        wifiTimeoutSec = wifiTimeoutMinutes * 60;
    } else {
        // Backward-compatible with a browser tab that was opened before this
        // firmware update and still submits the former seconds field.
        wifiTimeoutSec =
            constrain(server.arg("wifi_timeout_sec").toInt(), 0, 86400);
    }

    int wifiAliveScheduleEnabled =
        server.arg("wifi_alive_schedule_enabled").toInt() ? 1 : 0;

    String wifiAliveSchedule = server.arg("wifi_alive_schedule");
    wifiAliveSchedule.trim();

    String wifiSsids[SENSORFORGE_WIFI_PROFILE_COUNT];
    String wifiPasses[SENSORFORGE_WIFI_PROFILE_COUNT];
    size_t outputProfile = 0;

    for (size_t formIndex = 0; formIndex < SENSORFORGE_WIFI_PROFILE_COUNT; ++formIndex) {
        String ssid = server.arg("wifi_ssid_" + String(formIndex + 1));
        ssid.trim();
        if (!ssid.length())
            continue;

        if (outputProfile >= SENSORFORGE_WIFI_PROFILE_COUNT)
            break;

        String pass = server.arg("wifi_pass_" + String(formIndex + 1));
        int sourceProfile = server.arg("wifi_source_" + String(formIndex + 1)).toInt();

        if (!pass.length() && sourceProfile >= 1 && sourceProfile <= SENSORFORGE_WIFI_PROFILE_COUNT) {
            const size_t sourceIndex = (size_t)(sourceProfile - 1);
            if (ssid == cfg_wifi_ssids[sourceIndex])
                pass = cfg_wifi_passes[sourceIndex];
        }

        wifiSsids[outputProfile] = ssid;
        wifiPasses[outputProfile] = pass;
        ++outputProfile;
    }

    for (size_t i = outputProfile; i < SENSORFORGE_WIFI_PROFILE_COUNT; ++i) {
        wifiSsids[i] = "";
        wifiPasses[i] = "";
    }

    String txPowerText = server.arg("wifi_tx_power_dbm");
    txPowerText.trim();
    char *txPowerEnd = nullptr;
    float wifiTxPowerDbm = strtof(txPowerText.c_str(), &txPowerEnd);
    if (!txPowerText.length() || !txPowerEnd || *txPowerEnd != '\0' || !isfinite(wifiTxPowerDbm)) {
        error = "Ungültige WiFi-Sendeleistung";
        return false;
    }

    int hotspotEnabled = server.arg("hotspot_enabled").toInt() ? 1 : 0;
    int hotspotFallbackEnabled = server.arg("hotspot_fallback_enabled").toInt() ? 1 : 0;

    String hotspotPassword = server.arg("hotspot_password");
    if (!hotspotPassword.length())
        hotspotPassword = cfg_hotspot_password;

    int hotspotHidden = server.arg("hotspot_hidden").toInt() ? 1 : 0;

    ConfigSaveResult result = configSaveWifiSettings(
        hostname,
        wifiOnSystemStart,
        wifiTimeoutSec,
        wifiAliveScheduleEnabled,
        wifiAliveSchedule,
        wifiSsids,
        wifiPasses,
        wifiTxPowerDbm,
        hotspotEnabled,
        hotspotFallbackEnabled,
        hotspotPassword,
        hotspotHidden,
        writeToSd,
        error
    );

    return result == CONFIG_SAVE_BOTH || result == CONFIG_SAVE_INTERNAL_ONLY;
}
