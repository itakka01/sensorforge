#include "webconfig_streamer.h"

#include "config.h"
#include "streamer.h"

#include <esp_heap_caps.h>

namespace {

String streamerUiHtmlEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 16);
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
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

String streamerUiFfmpegDrawtextEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 16);
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        if (c == '\\') out += "\\\\";
        else if (c == '\'') out += "\\'";
        else if (c == ':') out += "\\:";
        else if (c == '%') out += "\\%";
        else if (c == '\r' || c == '\n') out += ' ';
        else out += c;
    }
    return out;
}

String streamerUiInfoButton(const String &title, const String &info)
{
    return
        "<button type='button' class='page-info-btn' aria-label='Information' "
        "data-title='" + streamerUiHtmlEscape(title) + "' data-info='" + streamerUiHtmlEscape(info) + "'>i</button>";
}

} // namespace

String webconfigStreamerDashboardHtml(uint8_t activeWebUiSessions)
{
    String html;
        html += "<section class='settings-section'><h3>Netzwerk-Streamer</h3>"
                "<p><strong>Status:</strong> " + String(streamerReady() ? "bereit" : "nicht gestartet") + "</p>"
                "<p>Kamera und optionales Audio gehören in diesem Betriebsmodus exklusiv dem Netzwerk-Streamer. "
                "Motion Recording, Power Shooter und die normale Aufnahme-Sleep-Automatik sind deaktiviert.</p>";
        html += "<p><strong>RTSP:</strong> <code>" + streamerUiHtmlEscape(streamerRtspUrl()) + "</code><br>"
                "<strong>HTTP-MJPEG (blank):</strong> <code>" + streamerUiHtmlEscape(streamerHttpUrl()) + "</code><br>"
                "<strong>Web-Viewer (+ Infos):</strong> <a href='" + streamerUiHtmlEscape(streamerHttpViewerUrl()) + "' target='_blank' rel='noopener'><code>" + streamerUiHtmlEscape(streamerHttpViewerUrl()) + "</code></a></p>";
        html += "<p><strong>RTSP-Clients:</strong> <span id='streamTopRtspCount'>" + String((unsigned)streamerRtspClientCount()) + "/2</span>" +
                " · <strong>HTTP-Clients:</strong> <span id='streamTopHttpCount'>" + String((unsigned)streamerHttpClientCount()) + "/2</span>" +
                "<br><strong>Audio:</strong> <span id='streamTopAudioState'>" + streamerUiHtmlEscape(streamerAudioStatus()) + "</span></p>";
        html += "<div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:10px;margin:12px 0'>"
                "<div style='border:1px solid #d7e2e8;border-radius:8px;padding:10px 12px;background:#fbfdfe'><strong>Web-Oberflächen / Systemwartung</strong><br><span id='webSessionCount'>" + String((unsigned)activeWebUiSessions) +
                "</span> aktiv</div>"
                "<div style='border:1px solid #d7e2e8;border-radius:8px;padding:10px 12px;background:#fbfdfe'><strong>Streaming-Verbindungen</strong><br><span id='streamClientCount'>" +
                String((unsigned)(streamerRtspClientCount() + streamerHttpClientCount())) +
                "</span> aktiv (max. 4)<br><span class='muted'>RTSP <span id='streamRtspCount'>" + String((unsigned)streamerRtspClientCount()) + "/2</span> · HTTP <span id='streamHttpCount'>" + String((unsigned)streamerHttpClientCount()) + "/2</span></span></div></div>";
        html += "<p><strong>Frames:</strong> <span id='streamLiveFrames'>" + String(streamerFramesCaptured()) + "</span>" +
                " · <strong>FPS:</strong> <span id='streamLiveFps'>" + String(streamerMeasuredFps(), 2) + "</span>" +
                " · <strong>Gesendet:</strong> <span id='streamLiveBytes'>" + String((unsigned long long)streamerBytesSent()) + "</span> Byte</p>";
        html += "<p><button class='button' type='button' id='streamDiagToggle' aria-expanded='false' aria-controls='streamDiagPanel'>Streamer-Status / Diagnose</button></p>";
        html += "<div id='streamDiagPanel' style='display:none;border:1px solid #8fb5c9;border-radius:8px;padding:12px;margin:8px 0 14px;background:#f7fbfd'>"
                "<p style='margin-top:0'><strong>Live-Streamerstatus</strong><br><span class='muted'>Aktualisierung alle 2 Sekunden, solange diese Seite sichtbar ist.</span></p>"
                "<p><strong>RTSP:</strong> <span id='diagRtspClients'>" + String((unsigned)streamerRtspClientCount()) + "/2</span> · "
                "<strong>HTTP-MJPEG:</strong> <span id='diagHttpClients'>" + String((unsigned)streamerHttpClientCount()) + "/2</span></p>"
                "<p><strong>Audio:</strong> <span id='diagAudioState'>" + streamerUiHtmlEscape(streamerAudioStatus()) + "</span><br>"
                "<strong>Audio-RTP-Pakete:</strong> <span id='diagAudioPackets'>0</span> · "
                "<strong>Audio gesendet:</strong> <span id='diagAudioBytes'>0</span> Byte<br><strong>Audio-Puffer:</strong> <span id='diagAudioBuffered'>0</span> / <span id='diagAudioCapacity'>0</span> KB · High-Water <span id='diagAudioHighWater'>0</span> KB<br><strong>Capture-Drops:</strong> <span id='diagAudioDropped'>0</span> Byte · <strong>Live-Abwurf:</strong> <span id='diagAudioLiveDiscard'>0</span> Byte · <strong>Drain:</strong> <span id='diagAudioDrainLast'>0</span> Pakete (max. <span id='diagAudioDrainMax'>0</span>)</p>"
                "<p><strong>Frames:</strong> <span id='diagFrames'>" + String(streamerFramesCaptured()) + "</span> · "
                "<strong>FPS:</strong> <span id='diagFps'>" + String(streamerMeasuredFps(), 2) + "</span> · "
                "<strong>Gesendet gesamt:</strong> <span id='diagBytes'>" + String((unsigned long long)streamerBytesSent()) + "</span> Byte</p>"
                "<p><strong>Thermal-Streaming:</strong> Ziel <span id='diagEffectiveFps'>–</span> fps · "
                "Stufe <span id='diagThermalLevel'>0</span> · CPU <span id='diagStreamerTemp'>–</span> °C<br>"
                "<span class='muted'>Drosselung erst nahe der 80-°C-Sicherheitsgrenze; der Thermal Guard selbst bleibt unverändert.</span></p>"
                "<p><strong>Video-Telemetrie:</strong> JPEG Ø <span id='diagJpegAvg'>0</span> KB · max. <span id='diagJpegMax'>0</span> KB<br>"
                "<strong>Videodatenrate:</strong> <span id='diagVideoKbit'>0</span> kbit/s · RTP/JPEG-Pakete <span id='diagRtpPackets'>0</span><br>"
                "<strong>RTSP Frame-Sendezeit:</strong> Ø <span id='diagRtspSendAvg'>0</span> ms · max. <span id='diagRtspSendMax'>0</span> ms<br>"
                "<span class='muted'>Die Datenrate wird im Browser aus den Zählerdifferenzen berechnet; die Firmware erzeugt dafür keinen zusätzlichen Netzwerkverkehr.</span></p>"
                "<div style='margin:10px 0'><strong>RTSP Live-Latenz / Rückstau</strong><br><span class='muted'>Kein zusätzlicher SensorForge-Framepuffer: angezeigt wird der Transportdruck je TCP-Client. RTCP-Sender-Reports synchronisieren Audio und Video auf eine gemeinsame Zeitreferenz.</span></div>"
                "<div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:10px;margin-bottom:12px'>"
                "<div id='diagRtspClient1Card' style='border:1px solid #d7e2e8;border-radius:8px;padding:10px 12px;background:#fbfdfe'><strong>RTSP Client 1</strong><br><span id='diagRtspClient1State'>nicht verbunden</span><br><span class='muted'>Sendezeit <span id='diagRtspClient1Send'>0</span> ms · Last <span id='diagRtspClient1Pressure'>0</span>%<br>Framefolge 1/<span id='diagRtspClient1Divider'>1</span> · ausgelassen <span id='diagRtspClient1Skipped'>0</span><br>RTCP SR Video <span id='diagRtspClient1RtcpV'>0</span> · Audio <span id='diagRtspClient1RtcpA'>0</span></span></div>"
                "<div id='diagRtspClient2Card' style='border:1px solid #d7e2e8;border-radius:8px;padding:10px 12px;background:#fbfdfe'><strong>RTSP Client 2</strong><br><span id='diagRtspClient2State'>nicht verbunden</span><br><span class='muted'>Sendezeit <span id='diagRtspClient2Send'>0</span> ms · Last <span id='diagRtspClient2Pressure'>0</span>%<br>Framefolge 1/<span id='diagRtspClient2Divider'>1</span> · ausgelassen <span id='diagRtspClient2Skipped'>0</span><br>RTCP SR Video <span id='diagRtspClient2RtcpV'>0</span> · Audio <span id='diagRtspClient2RtcpA'>0</span></span></div>"
                "</div>"
                "<p><strong>Self-Healing:</strong> Kamera <span id='diagCameraRecoveries'>0</span> · "
                "Audio <span id='diagAudioRecoveries'>0</span> · Netzwerk <span id='diagNetworkRecoveries'>0</span> Recoveries<br>"
                "<strong>Socket-Stalls:</strong> <span id='diagSocketStalls'>0</span> · "
                "fehlgeschlagene Kamera-Recovery-Versuche <span id='diagRecoveryFailures'>0</span><br>"
                "<strong>Letzter erfolgreicher Frame:</strong> vor <span id='diagLastFrameAge'>0</span> ms · "
                "<strong>aktuelle Capture-Fehler:</strong> <span id='diagCaptureFailures'>0</span><br>"
                "<strong>Speichertrend:</strong> <span id='diagMemoryWarning'>unauffällig</span> · "
                "<strong>Reset-Ursache:</strong> <span id='diagResetReason'>–</span><br>"
                "<strong>Langzeit gesamt:</strong> Kamera <span id='diagPersistCamera'>0</span> · Audio <span id='diagPersistAudio'>0</span> · Netzwerk <span id='diagPersistNetwork'>0</span> · Socket-Stalls <span id='diagPersistSocket'>0</span><br>"
                "<span class='muted'>Letzter Recovery-Grund: <span id='diagRecoveryReason'>–</span><br>"
                "Vorheriger kontrollierter Streamer-Neustart: <span id='diagPreviousReset'>–</span></span></p>"
                "<p class='muted' style='margin-bottom:0'>Die Health-Checks laufen langsam und ereignisorientiert. Ein einzelner Client- oder Audiofehler wird lokal behandelt; Netzwerk/Kamera werden erst nach bestätigtem Stall neu initialisiert. Ein Board-Neustart ist nur die letzte Eskalationsstufe.</p>"
                "</div>";
        html += "<p><strong>Systemressourcen:</strong> interner Heap <span id='heapFreeKb'>" +
                String((unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U)) +
                " KB</span> frei (Minimum seit Boot <span id='heapMinKb'>" +
                String((unsigned long)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U)) +
                " KB</span>) · PSRAM <span id='psramFreeKb'>" +
                String((unsigned long)(ESP.getFreePsram() / 1024U)) + " KB</span> frei</p>";
        if (streamerLastError().length()) {
            html += "<div class='flash-notice error'><strong>Streamer-Fehler</strong><span>" +
                    streamerUiHtmlEscape(streamerLastError()) + "</span></div>";
        }
        html += "</section>";
        html += "<script>(function(){"
                "var b=document.getElementById('streamDiagToggle'),p=document.getElementById('streamDiagPanel');"
                "if(!b||!p)return;"
                "var prevVideoBytes=null,prevVideoTs=0;"
                "function set(id,v){var e=document.getElementById(id);if(e)e.textContent=v;}"
                "function rtspClientDiag(n,s){var on=!!s['rtsp_client'+n+'_connected'],d=Math.max(1,Number(s['rtsp_client'+n+'_divider']||1)),ms=Math.max(0,Number(s['rtsp_client'+n+'_last_send_ms']||0)),fps=Math.max(1,Number(s.effective_fps||1)),budget=1000/fps,pr=Math.round(ms*100/budget),card=document.getElementById('diagRtspClient'+n+'Card'),state='nicht verbunden',bg='#fbfdfe',bd='#d7e2e8';if(on){if(d>1){state='holt auf · reduzierte Framefolge';bg='#fff7e6';bd='#e0ad53';}else if(pr>=60){state='erhöhte Transportlast';bg='#fff1f0';bd='#d96b64';}else{state='live / unauffällig';bg='#eef9f0';bd='#75b77d';}}set('diagRtspClient'+n+'State',state);set('diagRtspClient'+n+'Send',ms.toFixed(1));set('diagRtspClient'+n+'Pressure',String(Math.max(0,pr)));set('diagRtspClient'+n+'Divider',String(d));set('diagRtspClient'+n+'Skipped',String(Number(s['rtsp_client'+n+'_skipped']||0)));set('diagRtspClient'+n+'RtcpV',String(Number(s['rtsp_client'+n+'_rtcp_video_sr']||0)));set('diagRtspClient'+n+'RtcpA',String(Number(s['rtsp_client'+n+'_rtcp_audio_sr']||0)));if(card){card.style.background=bg;card.style.borderColor=bd;}}"
                "function apply(s){if(!s)return;"
                "set('diagRtspClients',String(Number(s.rtsp_clients)||0)+'/2');"
                "set('diagHttpClients',String(Number(s.http_clients)||0)+'/2');"
                "var ast=s.audio_active?'aktiv':(s.audio_available?'verfügbar, aber nicht aktiv':'nicht verfügbar');set('diagAudioState',ast);"
                "set('diagAudioPackets',String(Number(s.audio_packets)||0));set('diagAudioBytes',String(Number(s.audio_bytes)||0));set('diagAudioBuffered',(Number(s.audio_buffered_bytes||0)/1024).toFixed(1));set('diagAudioCapacity',(Number(s.audio_buffer_capacity||0)/1024).toFixed(1));set('diagAudioHighWater',(Number(s.audio_buffer_high_water||0)/1024).toFixed(1));set('diagAudioDropped',String(Number(s.audio_dropped_bytes)||0));set('diagAudioLiveDiscard',String(Number(s.audio_live_discard_bytes)||0));set('diagAudioDrainLast',String(Number(s.audio_drain_packets_last)||0));set('diagAudioDrainMax',String(Number(s.audio_drain_packets_max)||0));"
                "set('diagFrames',String(Number(s.frames)||0));set('diagFps',Number(s.fps||0).toFixed(2));set('diagBytes',String(Number(s.bytes_sent)||0));"
                "set('diagEffectiveFps',String(Number(s.effective_fps)||0));set('diagThermalLevel',String(Number(s.thermal_throttle_level)||0));var tc=Number(s.streamer_cpu_temp_c);set('diagStreamerTemp',Number.isFinite(tc)?tc.toFixed(1):'–');"
                "set('diagJpegAvg',(Number(s.jpeg_avg_bytes||0)/1024).toFixed(1));set('diagJpegMax',(Number(s.jpeg_max_bytes||0)/1024).toFixed(1));set('diagRtpPackets',String(Number(s.rtsp_video_packets)||0));set('diagRtspSendAvg',Number(s.rtsp_send_avg_ms||0).toFixed(2));set('diagRtspSendMax',Number(s.rtsp_send_max_ms||0).toFixed(2));var vb=Number(s.rtsp_video_bytes||0)+Number(s.http_video_bytes||0),tn=Date.now();if(prevVideoBytes!==null&&tn>prevVideoTs){set('diagVideoKbit',Math.max(0,(vb-prevVideoBytes)*8/(tn-prevVideoTs)).toFixed(0));}prevVideoBytes=vb;prevVideoTs=tn;"
                "rtspClientDiag(1,s);rtspClientDiag(2,s);"
                "set('diagCameraRecoveries',String(Number(s.camera_recoveries)||0));set('diagAudioRecoveries',String(Number(s.audio_recoveries)||0));set('diagNetworkRecoveries',String(Number(s.network_recoveries)||0));"
                "set('diagSocketStalls',String(Number(s.socket_stalls)||0));set('diagRecoveryFailures',String(Number(s.camera_recovery_failures)||0));"
                "set('diagLastFrameAge',String(Number(s.last_frame_age_ms)||0));set('diagCaptureFailures',String(Number(s.capture_failures)||0));set('diagRecoveryReason',s.last_recovery_reason||'–');"
                "set('diagMemoryWarning',s.memory_warning?'auffällig':'unauffällig');set('diagResetReason',s.reset_reason||'–');set('diagPreviousReset',s.previous_controlled_reset||'–');"
                "set('diagPersistCamera',String(Number(s.persistent_camera_recoveries)||0));set('diagPersistAudio',String(Number(s.persistent_audio_recoveries)||0));set('diagPersistNetwork',String(Number(s.persistent_network_recoveries)||0));set('diagPersistSocket',String(Number(s.persistent_socket_stalls)||0));"
                "set('streamLiveFrames',String(Number(s.frames)||0));set('streamLiveFps',Number(s.fps||0).toFixed(2));set('streamLiveBytes',String(Number(s.bytes_sent)||0));"
                "set('streamTopRtspCount',String(Number(s.rtsp_clients)||0)+'/2');set('streamTopHttpCount',String(Number(s.http_clients)||0)+'/2');set('streamTopAudioState',s.audio_active?'aktiv':(s.audio_available?'verfügbar - startet bei RTSP-Anforderung':'nicht verfügbar'));var sc=document.getElementById('streamClientCount');if(sc)sc.textContent=String((Number(s.rtsp_clients)||0)+(Number(s.http_clients)||0));var src=document.getElementById('streamRtspCount');if(src)src.textContent=String(Number(s.rtsp_clients)||0)+'/2';var shc=document.getElementById('streamHttpCount');if(shc)shc.textContent=String(Number(s.http_clients)||0)+'/2';"
                "}"
                "function poll(){if(document.hidden)return;fetch('/streamer_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)throw new Error();return r.json();}).then(apply).catch(function(){});}"
                "b.addEventListener('click',function(){var show=p.style.display==='none';p.style.display=show?'block':'none';b.setAttribute('aria-expanded',show?'true':'false');b.textContent=show?'Streamer-Status / Diagnose ausblenden':'Streamer-Status / Diagnose';if(show)poll();});"
                "setInterval(poll,2000);document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});poll();"
                "})();</script>";
    return html;
}

String webconfigStreamerOperatingModeHtml(
    bool configuredStreamerMode,
    int displayedStreamerRtspEnabled,
    int displayedStreamerHttpEnabled
)
{
    String html;
    String operatingModeInfo =
        "Hier legst du den zentralen Betriebsmodus von SensorForge fest. Die Aufnahmevarianten arbeiten wie bisher. "
        "Im Netzwerk-Streamer-Modus gehören Kamera und optionales Audio exklusiv dem Streamer; automatische Aufnahme, Power Shooter und normale Aufnahme-Sleep-Automatik bleiben inaktiv. "
        "Ein Wechsel zum oder vom Netzwerk-Streamer wird gespeichert und erst nach einem Neustart wirksam.";
    String streamerSettingsInfo =
        "Der Netzwerk-Streamer verwendet die bereits vorhandenen Kamera- und Audioeinstellungen von SensorForge. "
        "Auflösung, Bildrate, JPEG-Qualität und Audioquelle werden deshalb nicht ein zweites Mal hier eingestellt. "
        "Diese Parameter bleiben auf den Kamera- bzw. Audioseiten konfigurierbar.";

    const String streamerRtspCommandUrl = streamerRtspUrl();
    const String streamerHttpCommandUrl = streamerHttpUrl();
    const String streamerHttpViewerCommandUrl = streamerHttpViewerUrl();
    const String rtspOverlayLabel = cfg_camera_overlay_text.length() ? cfg_camera_overlay_text : (cfg_camera_display_name.length() ? cfg_camera_display_name : String("SensorForge"));
    const String rtspOverlayFilter = "drawtext=text='" + streamerUiFfmpegDrawtextEscape(rtspOverlayLabel) + "':x=18:y=18:fontsize=24:fontcolor=white:box=1:boxcolor=black@0.55,drawtext=text='%{localtime\\:%d.%m.%Y} %{localtime\\:%H}\\:%{localtime\\:%M}\\:%{localtime\\:%S}':x=18:y=h-th-18:fontsize=22:fontcolor=white:box=1:boxcolor=black@0.55";
    const String streamerFfplayOverlayCommand = "ffplay -rtsp_transport tcp -vf \"" + rtspOverlayFilter + "\" \"" + streamerRtspCommandUrl + "\"";
    const String streamerFfplayCommand = "ffplay -rtsp_transport tcp \"" + streamerRtspCommandUrl + "\"";
    const String streamerVlcCommand = "vlc \"" + streamerRtspCommandUrl + "\"";
    const String streamerMpvCommand = "mpv --demuxer-lavf-o=rtsp_transport=tcp \"" + streamerRtspCommandUrl + "\"";
    const String streamerHttpFfplayCommand = "ffplay \"" + streamerHttpCommandUrl + "\"";
    const String streamerHttpVlcCommand = "vlc \"" + streamerHttpCommandUrl + "\"";
    const String streamerHttpMpvCommand = "mpv \"" + streamerHttpCommandUrl + "\"";

    String streamerTransportInfo =
        "RTSP ist die bevorzugte Wahl für klassische Video- und Überwachungsprogramme. "
        "Typische Anwendungen sind VLC, ffmpeg sowie NVR-/Überwachungssysteme wie Frigate oder Shinobi. RTSP wird von normalen Webbrowsern wie Chrome nicht direkt abgespielt; optionales SensorForge-Audio wird über RTSP übertragen.\n\n"
        "HTTP-MJPEG ist besonders einfach für Webbrowser, Home Assistant, Dashboards und eigene HTTP-Integrationen. Der Link kann direkt in Chrome/Firefox geöffnet werden und liefert Video ohne Audio. "
        "Es ist unkompliziert einzubinden, aber bei dauerhaftem Betrieb meist weniger effizient als RTSP.\n\n"
        "Beide Stream-Arten dürfen gleichzeitig aktiviert sein. SensorForge verwendet dafür denselben Kameraframe. "
        "Pro Stream-Art sind aktuell maximal zwei gleichzeitig verbundene Clients vorgesehen; alle Clients teilen denselben Kameraframe.";
    html +=
        "<div id='operating-mode' style='margin:0 0 18px 0;padding:14px;border:1px solid #d8dee6;border-radius:8px;background:#f8fbff'>"
        "<div style='display:flex;align-items:center;gap:6px;flex-wrap:wrap'><b>Betriebsmodus</b>" +
        streamerUiInfoButton("Betriebsmodus", operatingModeInfo) +
        "</div>"
        "<select id='cfgOperatingMode' name='operating_mode_ui' style='min-width:320px;max-width:100%;margin-top:8px'>"
        "<option value='off'" + String(!configuredStreamerMode && !cfg_motion_recording_enabled && !cfg_shooter_enabled ? " selected" : "") + ">Aus - keine automatische Aufnahme</option>"
        "<option value='motion'" + String(!configuredStreamerMode && cfg_motion_recording_enabled && !cfg_shooter_enabled ? " selected" : "") + ">Normal Recording - Motion/Alarm</option>"
        "<option value='shooter'" + String(!configuredStreamerMode && !cfg_motion_recording_enabled && cfg_shooter_enabled ? " selected" : "") + ">Power Shooter standalone</option>"
        "<option value='motion_shooter'" + String(!configuredStreamerMode && cfg_motion_recording_enabled && cfg_shooter_enabled ? " selected" : "") + ">Normal Recording + Power Shooter</option>"
        "<option value='streamer'" + String(configuredStreamerMode ? " selected" : "") + ">Netzwerk-Streamer</option>"
        "</select>"
        "<div id='cfgStreamerOptions' style='margin-top:14px;padding:14px;border:1px solid #8fb5c9;border-radius:8px;background:#f7fbfd'>"
        "<div style='display:flex;align-items:center;gap:6px;flex-wrap:wrap'><b>Netzwerk-Streamer</b>" +
        streamerUiInfoButton("Netzwerk-Streamer", streamerSettingsInfo) +
        "</div>"
        "<div style='display:flex;align-items:center;gap:6px;flex-wrap:wrap;margin:8px 0 12px 0'><span class='muted'>Wähle mindestens eine Ausgabe. Beide können gleichzeitig verwendet werden.</span>" +
        streamerUiInfoButton("Welche Stream-Art brauche ich?", streamerTransportInfo) +
        "</div>"
        "<div style='padding:10px 12px;border:1px solid #d6e1e8;border-radius:9px;background:#fff;margin-bottom:10px'>"
        "<label class='stream-toggle' for='cfgStreamerRtsp'><input id='cfgStreamerRtsp' type='checkbox' name='streamer_rtsp_enabled' value='1'" + String(displayedStreamerRtspEnabled ? " checked" : "") + "><span class='stream-toggle-title'>RTSP-Stream</span><span id='cfgStreamerRtspState' class='stream-toggle-state " + String(displayedStreamerRtspEnabled ? "on" : "off") + "'>" + String(displayedStreamerRtspEnabled ? "AKTIV" : "INAKTIV") + "</span></label>"
        "<div class='muted' style='margin:4px 0 0 24px'>Für VLC, ffmpeg, NVR und Überwachungssoftware wie Frigate oder Shinobi. <strong>Empfohlen für Videoüberwachung und dauerhafte Integration.</strong> Nicht direkt in Chrome/Firefox abspielbar; optional mit Audio.</div>"
        "<div id='cfgStreamerRtspDetails' style='margin:7px 0 0 24px" + String(displayedStreamerRtspEnabled ? "" : ";display:none") + "'>"
        "<div><strong>Adresse:</strong> <code>" + streamerUiHtmlEscape(streamerRtspCommandUrl) + "</code></div>"
        "<div style='margin-top:10px'><strong>Stream öffnen:</strong></div>"
        "<div class='muted' style='margin:3px 0 7px 0'>Die folgenden Befehle können direkt in einem Terminal verwendet werden.</div>"
        "<div style='display:grid;gap:7px;max-width:900px'>"
        "<div style='display:flex;gap:7px;align-items:center;flex-wrap:wrap'><span style='min-width:55px'><strong>ffplay</strong></span><code style='flex:1;min-width:240px;overflow-wrap:anywhere'>" + streamerUiHtmlEscape(streamerFfplayCommand) + "</code><button type='button' class='button cfgStreamerCopyCmd' data-copy='" + streamerUiHtmlEscape(streamerFfplayCommand) + "'>Kopieren</button></div>"
        "<div style='display:flex;gap:7px;align-items:center;flex-wrap:wrap'><span style='min-width:55px'><strong>VLC</strong></span><code style='flex:1;min-width:240px;overflow-wrap:anywhere'>" + streamerUiHtmlEscape(streamerVlcCommand) + "</code><button type='button' class='button cfgStreamerCopyCmd' data-copy='" + streamerUiHtmlEscape(streamerVlcCommand) + "'>Kopieren</button></div>"
        "<div style='display:flex;gap:7px;align-items:center;flex-wrap:wrap'><span style='min-width:55px'><strong>mpv</strong></span><code style='flex:1;min-width:240px;overflow-wrap:anywhere'>" + streamerUiHtmlEscape(streamerMpvCommand) + "</code><button type='button' class='button cfgStreamerCopyCmd' data-copy='" + streamerUiHtmlEscape(streamerMpvCommand) + "'>Kopieren</button></div>"
        "<div style='display:flex;gap:7px;align-items:center;flex-wrap:wrap;margin-top:7px'><span style='min-width:110px'><strong>ffplay + Overlay</strong></span><code style='flex:1;min-width:240px;overflow-wrap:anywhere'>" + streamerUiHtmlEscape(streamerFfplayOverlayCommand) + "</code><button type='button' class='button cfgStreamerCopyCmd' data-copy='" + streamerUiHtmlEscape(streamerFfplayOverlayCommand) + "'>Kopieren</button></div>"
        "<div class='muted' style='margin-top:6px'>Das RTSP-Overlay wird ausschließlich vom Player-PC erzeugt (ffplay/FFmpeg drawtext); SensorForge verändert oder rekodiert keine JPEG-Frames.</div>"
        "</div>"
        "<div class='muted' style='margin-top:7px'>SensorForge überträgt RTSP als RTP über TCP. Bei ffplay und mpv wird TCP deshalb ausdrücklich vorgegeben. VLC wird mit der normalen RTSP-Adresse gestartet, da nicht jede VLC-Version dieselbe TCP-Kommandozeilenoption unterstützt.</div>"
        "</div>"
        "</div>"
        "<div style='padding:10px 12px;border:1px solid #d6e1e8;border-radius:9px;background:#fff'>"
        "<label class='stream-toggle' for='cfgStreamerHttp'><input id='cfgStreamerHttp' type='checkbox' name='streamer_http_mjpeg_enabled' value='1'" + String(displayedStreamerHttpEnabled ? " checked" : "") + "><span class='stream-toggle-title'>Browser-Stream (HTTP-MJPEG)</span><span id='cfgStreamerHttpState' class='stream-toggle-state " + String(displayedStreamerHttpEnabled ? "on" : "off") + "'>" + String(displayedStreamerHttpEnabled ? "AKTIV" : "INAKTIV") + "</span></label>"
        "<div class='muted' style='margin:4px 0 0 24px'>Für Webbrowser, Home Assistant, einfache Dashboards und eigene Integrationen. <strong>Direkt in Chrome/Firefox nutzbar; Video ohne Audio.</strong> Einfach zu verwenden, aber weniger effizient als RTSP.</div>"
        "<div id='cfgStreamerHttpDetails' style='margin:7px 0 0 24px" + String(displayedStreamerHttpEnabled ? "" : ";display:none") + "'>"
        "<div><strong>Adresse:</strong> <code>" + streamerUiHtmlEscape(streamerHttpCommandUrl) + "</code></div>"
        "<div style='margin-top:10px'><strong>Stream öffnen:</strong></div>"
        "<div class='muted' style='margin:3px 0 7px 0'>Die URL kann direkt im Browser geöffnet oder mit einem der folgenden Programme verwendet werden.</div>"
        "<div style='display:grid;gap:7px;max-width:900px'>"
        "<div style='display:flex;gap:7px;align-items:center;flex-wrap:wrap'><span style='min-width:105px'><strong>Browser blank</strong></span><code style='flex:1;min-width:240px;overflow-wrap:anywhere'>" + streamerUiHtmlEscape(streamerHttpCommandUrl) + "</code><button type='button' class='button cfgStreamerCopyCmd' data-copy='" + streamerUiHtmlEscape(streamerHttpCommandUrl) + "'>Kopieren</button><a class='button' href='" + streamerUiHtmlEscape(streamerHttpCommandUrl) + "' target='_blank' rel='noopener'>Öffnen</a></div>"
        "<div style='display:flex;gap:7px;align-items:center;flex-wrap:wrap'><span style='min-width:105px'><strong>Browser + Info</strong></span><code style='flex:1;min-width:240px;overflow-wrap:anywhere'>" + streamerUiHtmlEscape(streamerHttpViewerCommandUrl) + "</code><button type='button' class='button cfgStreamerCopyCmd' data-copy='" + streamerUiHtmlEscape(streamerHttpViewerCommandUrl) + "'>Kopieren</button><a class='button' href='" + streamerUiHtmlEscape(streamerHttpViewerCommandUrl) + "' target='_blank' rel='noopener'>Öffnen</a></div>"
        "<div style='display:flex;gap:7px;align-items:center;flex-wrap:wrap'><span style='min-width:55px'><strong>ffplay</strong></span><code style='flex:1;min-width:240px;overflow-wrap:anywhere'>" + streamerUiHtmlEscape(streamerHttpFfplayCommand) + "</code><button type='button' class='button cfgStreamerCopyCmd' data-copy='" + streamerUiHtmlEscape(streamerHttpFfplayCommand) + "'>Kopieren</button></div>"
        "<div style='display:flex;gap:7px;align-items:center;flex-wrap:wrap'><span style='min-width:55px'><strong>VLC</strong></span><code style='flex:1;min-width:240px;overflow-wrap:anywhere'>" + streamerUiHtmlEscape(streamerHttpVlcCommand) + "</code><button type='button' class='button cfgStreamerCopyCmd' data-copy='" + streamerUiHtmlEscape(streamerHttpVlcCommand) + "'>Kopieren</button></div>"
        "<div style='display:flex;gap:7px;align-items:center;flex-wrap:wrap'><span style='min-width:55px'><strong>mpv</strong></span><code style='flex:1;min-width:240px;overflow-wrap:anywhere'>" + streamerUiHtmlEscape(streamerHttpMpvCommand) + "</code><button type='button' class='button cfgStreamerCopyCmd' data-copy='" + streamerUiHtmlEscape(streamerHttpMpvCommand) + "'>Kopieren</button></div>"
        "</div>"
        "<div class='muted' style='margin-top:7px'>HTTP-MJPEG enthält nur Video. Für Audio verwende den RTSP-Stream.</div>"
        "</div>"
        "</div>"
        "<div id='cfgStreamerTransportWarning' class='flash-notice error' style='display:none;margin-top:10px'><strong>Keine Stream-Ausgabe gewählt</strong><span>Aktiviere RTSP oder Browser-Stream. Beide dürfen auch gleichzeitig aktiv sein.</span></div>"
        "<p><strong>Audio:</strong> " + streamerUiHtmlEscape(streamerAudioStatus()) + "</p>"
        "<div id='cfgStreamerStress' style='margin:14px 0 10px;padding:12px;border:1px solid #d6e1e8;border-radius:9px;background:#fff'>"
        "<div><strong>Streamer-Stresstest / Leistungsanalyse</strong></div>"
        "<div class='muted' style='margin-top:5px'>Manueller Messmodus für Stromaufnahme und Temperatur. Die Tests verwenden reale SensorForge-Pfade und verändern keine gespeicherten Einstellungen. Thermal Guard und dynamisches FPS-Throttling bleiben aktiv.</div>"
        "<div style='display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:10px'><label>Dauer <select id='cfgStressSeconds'><option value='30'>30 s</option><option value='60' selected>60 s</option><option value='120'>120 s</option></select></label>"
        "<button type='button' class='button' id='cfgStressCamera'>Kamera/JPEG</button>"
        "<button type='button' class='button' id='cfgStressHttp'>HTTP-MJPEG</button>"
        "<button type='button' class='button' id='cfgStressObserve'>Aktuellen Stream messen</button>"
        "<button type='button' class='button' id='cfgStressStop'>Stop</button></div>"
        "<div id='cfgStressStatus' class='muted' style='margin-top:9px'>Bereit. Für Kamera/JPEG und HTTP bitte vorher andere Streamclients schließen. Für RTSP(+Audio) zuerst den Player starten und dann ‘Aktuellen Stream messen’ wählen.</div>"
        "<img id='cfgStressHttpImg' alt='' style='position:absolute;left:-10000px;top:-10000px;width:1px;height:1px'>"
        "<div class='muted' style='margin-top:6px'>Stromaufnahme extern am Messgerät ablesen/notieren. START und END werden mit Dauer, Temperatur, Frames, JPEG-Größe, Videodatenrate, RTP-Paketen, RTSP-Sendezeit und Audiozählern im Log protokolliert.</div>"
        "</div>"
        "<p class='muted'>Im Streamer-Modus sind Recording, Power Shooter und die normale Aufnahme-Sleep-Automatik nicht aktiv. "
        "Der Wechsel zum oder vom Streamer wird erst nach einem Neustart wirksam.</p>"
        "<a class='button' href='/preview'>Kameraeinstellungen</a>"
        "</div>"
        "</div>"
        "<script>(function(){"
        "var m=document.getElementById('cfgOperatingMode'),p=document.getElementById('cfgStreamerOptions'),r=document.getElementById('cfgStreamerRtsp'),h=document.getElementById('cfgStreamerHttp'),rs=document.getElementById('cfgStreamerRtspState'),hs=document.getElementById('cfgStreamerHttpState'),rd=document.getElementById('cfgStreamerRtspDetails'),hd=document.getElementById('cfgStreamerHttpDetails'),w=document.getElementById('cfgStreamerTransportWarning'),f=document.getElementById('configForm'),sc=document.getElementById('cfgStressCamera'),sh=document.getElementById('cfgStressHttp'),so=document.getElementById('cfgStressObserve'),ss=document.getElementById('cfgStressStop'),sd=document.getElementById('cfgStressSeconds'),st=document.getElementById('cfgStressStatus'),si=document.getElementById('cfgStressHttpImg');"
        "function setState(el,on){if(!el)return;el.textContent=on?'AKTIV':'INAKTIV';el.classList.toggle('on',on);el.classList.toggle('off',!on);}function transportDetails(){if(rd)rd.style.display=r.checked?'block':'none';if(hd)hd.style.display=h.checked?'block':'none';setState(rs,r.checked);setState(hs,h.checked);}"
        "function copyText(v){if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(v).catch(function(){});return;}var t=document.createElement('textarea');t.value=v;t.setAttribute('readonly','');t.style.position='fixed';t.style.opacity='0';document.body.appendChild(t);t.select();try{document.execCommand('copy');}catch(x){}document.body.removeChild(t);}"
        "document.querySelectorAll('.cfgStreamerCopyCmd').forEach(function(b){b.addEventListener('click',function(){copyText(b.getAttribute('data-copy')||'');var old=b.textContent;b.textContent='Kopiert';setTimeout(function(){b.textContent=old;},1200);});});"
        "function valid(){var ok=r.checked||h.checked;if(w)w.style.display=(m.value==='streamer'&&!ok)?'block':'none';transportDetails();return ok;}"
        "function sync(ev){p.style.display=m.value==='streamer'?'block':'none';if(m.value==='streamer'&&ev&&!r.checked&&!h.checked)r.checked=true;valid();}"
        "m.addEventListener('change',function(){sync(true);});r.addEventListener('change',valid);h.addEventListener('change',valid);"
        "f.addEventListener('submit',function(ev){if(m.value==='streamer'&&!valid()){ev.preventDefault();w.scrollIntoView({behavior:'smooth',block:'center'});}});"
        "function stressMsg(v,bad){if(!st)return;st.textContent=v;st.style.color=bad?'#b00020':'';}function stressPost(url,data){return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data),cache:'no-store',credentials:'same-origin'}).then(function(x){return x.json().then(function(j){if(!x.ok||!j.ok)throw new Error(j.error||('HTTP '+x.status));return j;});});}"
        "function startStress(mode){var sec=Number(sd&&sd.value||60);stressPost('/streamer_stress_start',{mode:mode,seconds:String(sec)}).then(function(){stressMsg('Messung läuft: '+mode+' · '+sec+' s. Stromaufnahme jetzt am Messgerät beobachten.',false);if(mode==='http'&&si){si.src='" + streamerUiHtmlEscape(streamerHttpCommandUrl) + "?stress='+Date.now();}}).catch(function(e){stressMsg('Stresstest nicht gestartet: '+e.message,true);});}"
        "if(sc)sc.addEventListener('click',function(){startStress('camera');});if(sh)sh.addEventListener('click',function(){startStress('http');});if(so)so.addEventListener('click',function(){startStress('observe');});if(ss)ss.addEventListener('click',function(){stressPost('/streamer_stress_stop',{}).then(function(){if(si)si.src='';stressMsg('Messung gestoppt; Ergebnis steht im Log.',false);}).catch(function(e){stressMsg('Stop fehlgeschlagen: '+e.message,true);});});"
        "function stressPoll(){fetch('/streamer_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'}).then(function(x){if(!x.ok)throw new Error();return x.json();}).then(function(j){if(j.stress_active){var rem=Math.ceil(Number(j.stress_remaining_ms||0)/1000);stressMsg('Messung läuft: '+String(j.stress_mode||'')+' · noch '+rem+' s. Stromaufnahme jetzt am Messgerät beobachten.',false);}else if(si&&si.src){si.src='';stressMsg('Messung beendet; Ergebnis steht im Log.',false);}}).catch(function(){});}setInterval(stressPoll,1000);"
        "sync(false);})();</script>";
    return html;
}
