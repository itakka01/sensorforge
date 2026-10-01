#include "webconfig_wifi.h"

#include "board_config.h"
#include "config.h"

#include <WebServer.h>
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

} // namespace

String webconfigWifiSettingsHtml(const String &notice)
{
    String html;

    html +=
        "<div class='page-title'><div><h2>WiFi Einstellungen</h2>"
        "<p>Netzwerk, Hotspot und Funkleistung</p></div></div>";

    html +=
        "<style>"
        ".config-field-grid{display:grid;grid-template-columns:minmax(190px,240px) minmax(220px,1fr);gap:8px 16px;align-items:center}"
        ".config-field-grid .config-label{font-weight:600}"
        ".config-field-grid input,.config-field-grid select{width:100%;max-width:430px;margin:0}"
        ".config-field-grid .config-control{min-width:0}"
        ".config-field-grid .config-note{grid-column:2;color:var(--muted);font-size:.86rem;margin-top:-4px;margin-bottom:4px}"
        ".wifi-profile-list{display:flex;flex-direction:column;gap:10px;margin-top:10px}"
        ".wifi-profile{border:1px solid var(--border);border-radius:10px;padding:12px;background:var(--card)}"
        ".wifi-profile-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:9px}"
        ".wifi-profile-title{font-weight:700}.wifi-profile-actions{display:flex;gap:6px;flex-wrap:wrap}"
        ".wifi-profile-actions button{padding:6px 9px;min-width:0}.wifi-profile-grid{display:grid;grid-template-columns:minmax(150px,210px) minmax(200px,1fr);gap:7px 12px;align-items:center}"
        ".wifi-profile-grid input{width:100%;max-width:430px;margin:0}.wifi-profile-note{color:var(--muted);font-size:.84rem;grid-column:2}"
        ".wifi-add-row{margin-top:10px}"
        "@media(max-width:720px){.config-field-grid{grid-template-columns:1fr;gap:4px}.config-field-grid .config-note{grid-column:1;margin-top:-2px;margin-bottom:8px}.config-field-grid input,.config-field-grid select{max-width:none}.wifi-profile-grid{grid-template-columns:1fr}.wifi-profile-note{grid-column:1}.wifi-profile-head{align-items:flex-start}.wifi-profile-actions{justify-content:flex-end}}"
        "</style>";

    if (notice == "saved") {
        html +=
            "<div class='flash-notice'><strong>WiFi-Einstellungen gespeichert.</strong>"
            "<span class='muted'>Netzwerkänderungen werden nach einem Neustart wirksam.</span></div>";
    }

    html +=
        "<form id='wifiSettingsForm' method='POST' action='/wifi_settings_save'>"
        "<section class='settings-section'><h3>Netzwerk</h3>"
        "<div class='config-field-grid'>";

    html +=
        "<div class='config-label'>Gerätename / Hotspot-Name</div>"
        "<div class='config-control'><input name='hostname' maxlength='63' autocomplete='off' value='" +
        wifiUiHtmlEscape(cfg_hostname) + "'></div>";

    html +=
        "<div class='config-label'>Netzwerkmodus</div>"
        "<div class='config-control'><select name='hotspot_enabled'>"
        "<option value='1'" + String(cfg_hotspot_enabled ? " selected" : "") + ">Eigener Hotspot / Access Point</option>"
        "<option value='0'" + String(!cfg_hotspot_enabled ? " selected" : "") + ">Externes WLAN verwenden</option>"
        "</select></div>"
        "<div class='config-note'>Bestimmt das Service-Netzwerk für Webinterface und Streamer. Es gibt keinen automatischen Wechsel auf den Hotspot, wenn das externe WLAN nicht erreichbar ist.</div>";

    html +=
        "<div class='config-label'>Externe WLAN-Netzwerke</div>"
        "<div class='config-control'><div class='config-note' style='grid-column:auto;margin:0'>Priorität von oben nach unten. SensorForge versucht beim Verbindungsaufbau das erste konfigurierte WLAN und danach die weiteren Einträge der Reihe nach.</div></div>";

    html += "</div><div id='wifiProfileList' class='wifi-profile-list'>";

    for (size_t i = 0; i < SENSORFORGE_WIFI_PROFILE_COUNT; ++i) {
        const bool configured = cfg_wifi_ssids[i].length() != 0;
        html +=
            "<div class='wifi-profile' data-slot='" + String(i + 1) + "'" +
            String((i == 0 || configured) ? "" : " style='display:none'") + ">"
            "<div class='wifi-profile-head'><span class='wifi-profile-title'>Priorität " + String(i + 1) + "</span>"
            "<span class='wifi-profile-actions'>"
            "<button type='button' class='wifi-up' title='Priorität erhöhen'>↑</button>"
            "<button type='button' class='wifi-down' title='Priorität verringern'>↓</button>"
            "<button type='button' class='wifi-delete'>Löschen</button>"
            "</span></div>"
            "<div class='wifi-profile-grid'>"
            "<div>SSID</div><div><input class='wifi-ssid' name='wifi_ssid_" + String(i + 1) + "' maxlength='32' autocomplete='off' value='" + wifiUiHtmlEscape(cfg_wifi_ssids[i]) + "' placeholder='Name des WLAN-Netzwerks'></div>"
            "<div>Passwort</div><div><input class='wifi-pass' type='password' name='wifi_pass_" + String(i + 1) + "' maxlength='63' autocomplete='new-password' value='' placeholder='Leer lassen = bestehendes Passwort behalten'></div>"
            "<input class='wifi-source' type='hidden' name='wifi_source_" + String(i + 1) + "' value='" + String(configured ? (int)i + 1 : 0) + "'>"
            "<div class='wifi-profile-note'>" + String(configured && cfg_wifi_passes[i].length() ? "Passwort gespeichert. Leer lassen, um es unverändert zu behalten." : "Kein gespeichertes Passwort.") + "</div>"
            "</div></div>";
    }

    html +=
        "</div><div class='wifi-add-row'><button id='wifiAddProfile' type='button'>+ WLAN hinzufügen</button></div>"
        "<div class='config-note' style='grid-column:1/-1;margin-top:6px'>Maximal 5 WLAN-Profile. Leere Einträge werden beim Speichern entfernt; die sichtbare Reihenfolge bestimmt die Priorität.</div>"
        "<div class='config-field-grid'>";

    html +=
        "<div class='config-label'>Hotspot-Passwort</div>"
        "<div class='config-control'><input type='password' name='hotspot_password' minlength='8' maxlength='63' "
        "autocomplete='new-password' value='' placeholder='Leer lassen = bestehende Einstellung behalten'></div>"
        "<div class='config-note'>Aktuell: " +
        String(cfg_hotspot_password.length() ? "passwortgeschützt" : "offen / kein Passwort") +
        ". Neues Passwort: 8 bis 63 Zeichen.</div>";

    html +=
        "<div class='config-label'>Hotspot-Name sichtbar</div>"
        "<div class='config-control'><select name='hotspot_hidden'>"
        "<option value='0'" + String(!cfg_hotspot_hidden ? " selected" : "") + ">Ja - Netzwerkname wird angezeigt</option>"
        "<option value='1'" + String(cfg_hotspot_hidden ? " selected" : "") + ">Nein - Netzwerkname wird versteckt</option>"
        "</select></div>";

    html += "</div></section>";

    html +=
        "<section class='settings-section'><h3>Start / Zeitabgleich</h3>"
        "<div class='config-field-grid'>"
        "<div class='config-label'>Zeitabgleich über externes WLAN beim Systemstart</div>"
        "<div class='config-control'><select name='wifi_on_system_start'>"
        "<option value='off'" + String(cfg_wifi_on_system_start == "off" ? " selected" : "") + ">Aus</option>"
        "<option value='on'" + String(cfg_wifi_on_system_start == "on" ? " selected" : "") + ">Ein</option>"
        "<option value='on_missing_time'" + String(cfg_wifi_on_system_start == "on_missing_time" ? " selected" : "") + ">Nur einschalten, wenn die Uhrzeit fehlt</option>"
        "</select></div>"
        "<div class='config-label'>WLAN automatisch ausschalten</div>"
        "<div class='config-control'><input name='wifi_timeout_sec' type='number' min='0' max='86400' value='" +
        String(cfg_wifi_timeout_sec) + "'></div>"
        "<div class='config-note'>Sekunden; 0 = automatische Abschaltung deaktiviert. Im Streamer-Modus bleibt das gewählte Netzwerk dauerhaft aktiv.</div>"
        "</div></section>";

    html +=
        "<section class='settings-section'><h3>Funkleistung</h3>"
        "<div class='config-field-grid'>"
        "<div class='config-label'>WiFi-Sendeleistung</div>"
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

    html +=
        "</select></div>"
        "<div class='config-note'>Gilt für externes WLAN und Hotspot. Board-Default: " +
        String(BOARD_WIFI_TX_POWER_DEFAULT_X10 / 10.0f, 1) + " dBm.</div>";

    int16_t effectiveTxSafeMinX10 = boardWifiTxPowerSafeMinimumX10();
    if (effectiveTxSafeMinX10 <= 110) {
        html +=
            "<div></div><div class='config-note' style='color:#b45309'><b>Achtung:</b> Sehr geringe Sendeleistung reduziert die Reichweitenreserve. "
            "Für dieses Board ist die kleinste freigegebene Stufe " +
            String(effectiveTxSafeMinX10 / 10.0f, 1) +
            " dBm. Niedrige Werte bis 11 dBm nur nach Reichweitentest verwenden.</div>";
    }

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
            "<div><b>" + String(rssi) + " dBm - " + qualityText + "</b></div>"
            "<div style='position:relative;height:14px;border-radius:7px;margin:8px 0 6px;background:linear-gradient(90deg,#c62828 0%,#f9a825 45%,#7cb342 72%,#2e7d32 100%);'>"
            "<span style='position:absolute;left:calc(" + String(markerPct) + "% - 2px);top:-4px;width:4px;height:22px;background:#111;border-radius:2px'></span></div>"
            "<div class='config-note'>Letzte Messung während der Verbindung mit dem externen WLAN. Empfehlung: <b>" +
            recommendation + "</b>. Keine automatische Änderung.</div>";
    } else {
        html +=
            "<div class='config-note'>Noch keine Empfangsmessung in dieser Laufzeit. RSSI ist nur bei aktiver Verbindung mit dem externen WLAN messbar.</div>";
    }

    html +=
        "</div></div></section>"
        "<div class='floating-save-space'></div>"
        "<div id='wifiSaveBar' class='floating-save-bar'>"
        "<span id='wifiSaveState' class='floating-save-state'>Keine ungespeicherten Änderungen</span>"
        "<button id='wifiSaveButton' type='submit' disabled>Speichern</button>"
        "</div></form>";

    html +=
        "<script>(function(){"
        "var f=document.getElementById('wifiSettingsForm'),b=document.getElementById('wifiSaveBar'),s=document.getElementById('wifiSaveState'),btn=document.getElementById('wifiSaveButton'),list=document.getElementById('wifiProfileList'),add=document.getElementById('wifiAddProfile');"
        "if(!f||!b||!s||!btn||!list)return;"
        "function dirty(){b.classList.add('dirty');s.textContent='Ungespeicherte Änderungen';btn.disabled=false;}"
        "function rows(){return Array.prototype.slice.call(list.querySelectorAll('.wifi-profile'));}"
        "function renumber(){rows().forEach(function(r,i){r.querySelector('.wifi-profile-title').textContent='Priorität '+(i+1);r.dataset.slot=i+1;['wifi-ssid','wifi-pass','wifi-source'].forEach(function(c){var e=r.querySelector('.'+c);if(e)e.name=(c==='wifi-ssid'?'wifi_ssid_':c==='wifi-pass'?'wifi_pass_':'wifi_source_')+(i+1);});});}"
        "function swap(a,c){if(!a||!c)return;var marker=document.createElement('span');a.parentNode.insertBefore(marker,a);c.parentNode.insertBefore(a,c);marker.parentNode.insertBefore(c,marker);marker.remove();renumber();dirty();}"
        "list.addEventListener('click',function(e){var r=e.target.closest('.wifi-profile');if(!r)return;var rs=rows().filter(function(x){return x.style.display!=='none';}),i=rs.indexOf(r);if(e.target.classList.contains('wifi-up')&&i>0)swap(r,rs[i-1]);else if(e.target.classList.contains('wifi-down')&&i>=0&&i<rs.length-1)swap(rs[i+1],r);else if(e.target.classList.contains('wifi-delete')){r.querySelector('.wifi-ssid').value='';r.querySelector('.wifi-pass').value='';r.querySelector('.wifi-source').value='0';r.style.display='none';list.appendChild(r);renumber();dirty();}});"
        "if(add)add.addEventListener('click',function(){var rs=rows();for(var i=0;i<rs.length;i++){if(rs[i].style.display==='none'){rs[i].style.display='';rs[i].querySelector('.wifi-ssid').focus();dirty();return;}}});"
        "f.addEventListener('input',dirty);f.addEventListener('change',dirty);"
        "f.addEventListener('submit',function(){renumber();btn.disabled=true;s.textContent='Speichert …';});"
        "})();</script>";

    return html;
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

    int wifiTimeoutSec = constrain(server.arg("wifi_timeout_sec").toInt(), 0, 86400);

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

    String hotspotPassword = server.arg("hotspot_password");
    if (!hotspotPassword.length())
        hotspotPassword = cfg_hotspot_password;

    int hotspotHidden = server.arg("hotspot_hidden").toInt() ? 1 : 0;

    ConfigSaveResult result = configSaveWifiSettings(
        hostname,
        wifiOnSystemStart,
        wifiTimeoutSec,
        wifiSsids,
        wifiPasses,
        wifiTxPowerDbm,
        hotspotEnabled,
        hotspotPassword,
        hotspotHidden,
        writeToSd,
        error
    );

    return result == CONFIG_SAVE_BOTH || result == CONFIG_SAVE_INTERNAL_ONLY;
}
