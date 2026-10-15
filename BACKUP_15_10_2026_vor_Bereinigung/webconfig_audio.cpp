#include "webconfig_audio.h"

#include "audio_capture.h"
#include "board_config.h"
#include "config.h"
#include "language.h"
#include "streamer.h"

namespace {

String audioUiHtmlEscape(const String &value)
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

String audioUiText(UiTextId id)
{
    return audioUiHtmlEscape(String(tr(id)));
}

} // namespace

void appendWebConfigAudioUi(String &html)
{
    html += "</div><div class='settings-section'><h3>Audio / Mikrofon</h3>";

    AudioCaptureCapabilities audioCaps =
        audioCaptureCapabilities();

    html +=
        "<div style='padding:14px;border:1px solid #8fb5c9;border-radius:8px;background:#f7fbfd'>"
        "<span class='muted'>" +
        audioUiText(UI_AUDIO_SIMPLE_HELP) +
        "</span><br><br>";

    html +=
        audioUiText(UI_AUDIO_ENABLE) +
        ": <select id='cfgAudioEnabled' name='audio_enabled' onchange='sfAudioUi()'>";
    html += "<option value='0'" +
            String(!cfg_audio_enabled ? " selected" : "") +
            ">0 - " + audioUiText(UI_AUDIO_OFF) + "</option>";
    html += "<option value='1'" +
            String(cfg_audio_enabled ? " selected" : "") +
            ">1 - " + audioUiText(UI_AUDIO_ON) + "</option>";
    html += "</select><br>";

    html +=
        "<small class='muted'>" +
        audioUiText(UI_AUDIO_MKV_REQUIRED) +
        "</small><br><br>";

    html +=
        "<button type='button' onclick=\"sfAudioAdvancedOpen()\">" +
        audioUiText(UI_AUDIO_ADVANCED_SETTINGS) +
        "</button> ";

    bool audioMicTestAllowed =
        audioCaps.available &&
        !streamerModeEnabled();

    html +=
        "<button id='sfAudioMicTestBtn' type='button' onclick=\"sfAudioMicTest()\"" +
        String(audioMicTestAllowed ? "" : " disabled") +
        ">" + audioUiText(UI_AUDIO_MIC_TEST_BUTTON) + "</button>";

    html +=
        "<div id='sfAudioMicTestPanel' style='margin-top:10px;padding:10px;border:1px solid #cfd8dc;border-radius:6px;background:#fff'>"
        "<small class='muted'>" + audioUiText(UI_AUDIO_MIC_TEST_HELP) + "</small>";

    if (streamerModeEnabled()) {
        html +=
            "<br><small style='color:#9a5a00'>" +
            audioUiText(UI_AUDIO_MIC_TEST_STREAMER_BLOCKED) +
            "</small>";
    }

    html +=
        "<div id='sfAudioMicTestStatus' style='margin-top:8px'>" +
        audioUiText(UI_AUDIO_MIC_TEST_READY) +
        "</div>"
        "<div id='sfAudioMicTestPlayback' style='display:none;margin-top:8px'>"
        "<audio id='sfAudioMicTestAudio' controls preload='none' style='width:100%;max-width:520px'></audio><br>"
        "<a id='sfAudioMicTestDownload' href='/audio_test_play?download=1'>" +
        audioUiText(UI_AUDIO_MIC_TEST_DOWNLOAD) +
        "</a></div></div>";

    if (!audioCaps.available) {
        html +=
            "<br><small style='color:#9a5a00'>" +
            audioUiText(UI_AUDIO_NO_INPUT) +
            "</small>";
    }

    html +=
        "<div id='sfAudioAdvancedModal' style='display:none;position:fixed;z-index:12000;inset:0;background:rgba(0,0,0,.48);padding:18px;overflow:auto'>"
        "<div style='max-width:720px;margin:5vh auto;background:#fff;border-radius:10px;padding:18px;box-shadow:0 10px 36px rgba(0,0,0,.3)'>"
        "<div style='display:flex;justify-content:space-between;align-items:center;gap:12px'>"
        "<h3 style='margin:0'>" + audioUiText(UI_AUDIO_ADVANCED_TITLE) + "</h3>"
        "<button type='button' onclick=\"sfAudioAdvancedClose()\">&times;</button>"
        "</div><p class='muted'>" + audioUiText(UI_AUDIO_ADVANCED_HELP) + "</p>";

    #if BOARD_HAS_INTEGRATED_MIC
    String boardAudioName = BOARD_INTEGRATED_MIC_NAME;
    #else
    String boardAudioName = tr(UI_NOT_DETECTED);
    #endif

    html +=
        "<p class='muted'>" +
        audioUiText(UI_AUDIO_SOURCE_BOARD_DEFAULT) +
        ": <b>" + audioUiHtmlEscape(boardAudioName) + "</b><br>" +
        audioUiText(UI_AUDIO_SOURCE) +
        " (" + audioUiText(UI_STATUS_ACTIVE) + "): <b>" +
        audioUiHtmlEscape(String(audioCaptureBackendName())) +
        "</b></p>";

    html +=
        audioUiText(UI_AUDIO_EXPERT_MODE) +
        ": <select id='cfgAudioExpertMode' name='audio_expert_mode' onchange='sfAudioUi()'>"
        "<option value='0'" +
        String(!cfg_audio_expert_mode ? " selected" : "") +
        ">0 - User</option>"
        "<option value='1'" +
        String(cfg_audio_expert_mode ? " selected" : "") +
        ">1 - Expert</option>"
        "</select><br>";

    html +=
        audioUiText(UI_AUDIO_SOURCE) +
        ": <select id='cfgAudioSource' name='audio_source' onchange='sfAudioUi()'>"
        "<option value='board_default'" +
        String(cfg_audio_source == "board_default" ? " selected" : "") +
        ">" + audioUiText(UI_AUDIO_SOURCE_BOARD_DEFAULT) + "</option>"
        "<option value='external'" +
        String(cfg_audio_source == "external" ? " selected" : "") +
        ">" + audioUiText(UI_AUDIO_SOURCE_EXTERNAL) + "</option>"
        "</select><br>";

    html +=
        "audio_sample_rate: <input name='audio_sample_rate' type='number' min='8000' max='96000' step='1000' value='" +
        String(cfg_audio_sample_rate) +
        "' style='width:110px'> Hz<br>";

    html += "audio_bits_per_sample: <select name='audio_bits_per_sample'>";
    html += "<option value='16'" + String(cfg_audio_bits_per_sample == 16 ? " selected" : "") + ">16 bit</option>";
    html += "<option value='24'" + String(cfg_audio_bits_per_sample == 24 ? " selected" : "") + ">24 bit</option>";
    html += "<option value='32'" + String(cfg_audio_bits_per_sample == 32 ? " selected" : "") + ">32 bit</option>";
    html += "</select><br>";

    html += "audio_channels: <select name='audio_channels'>";
    html += "<option value='1'" + String(cfg_audio_channels == 1 ? " selected" : "") + ">1 - mono</option>";
    html += "<option value='2'" + String(cfg_audio_channels == 2 ? " selected" : "") + ">2 - stereo</option>";
    html += "</select><br>";

    html +=
        "<div id='cfgAudioExpertPanel' style='margin-top:12px;padding:12px;border:1px dashed #78909c;border-radius:6px'>"
        "<b>" + audioUiText(UI_AUDIO_EXTERNAL_PINS) + "</b><br>"
        "<small class='muted'>" + audioUiText(UI_AUDIO_EXPERT_HELP) + "</small><br>"
        "<small style='color:#9a5a00'>" + audioUiText(UI_AUDIO_GPIO_WARNING) + "</small><br><br>" +
        audioUiText(UI_AUDIO_BACKEND) +
        ": <select id='cfgAudioBackend' name='audio_backend' onchange='sfAudioUi()'>"
        "<option value='pdm'" +
        String(cfg_audio_backend == "pdm" ? " selected" : "") +
        ">" + audioUiText(UI_AUDIO_BACKEND_PDM) + "</option>"
        "<option value='i2s'" +
        String(cfg_audio_backend == "i2s" ? " selected" : "") +
        ">" + audioUiText(UI_AUDIO_BACKEND_I2S) + "</option>"
        "</select><br>";

    html +=
        "<div id='cfgAudioPdmPanel' style='margin-top:8px'>"
        "audio_pdm_clk_pin: <input name='audio_pdm_clk_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_pdm_clk_pin) +
        "' style='width:80px'><br>"
        "audio_pdm_data_pin: <input name='audio_pdm_data_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_pdm_data_pin) +
        "' style='width:80px'><br>"
        "</div>";

    html +=
        "<div id='cfgAudioI2sPanel' style='margin-top:8px'>"
        "audio_i2s_bclk_pin: <input name='audio_i2s_bclk_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_i2s_bclk_pin) +
        "' style='width:80px'><br>"
        "audio_i2s_ws_pin: <input name='audio_i2s_ws_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_i2s_ws_pin) +
        "' style='width:80px'><br>"
        "audio_i2s_data_pin: <input name='audio_i2s_data_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_i2s_data_pin) +
        "' style='width:80px'><br>"
        "audio_i2s_mclk_pin: <input name='audio_i2s_mclk_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_i2s_mclk_pin) +
        "' style='width:80px'> <small class='muted'>-1 = unused</small><br>" +
        audioUiText(UI_AUDIO_I2S_SLOT) +
        ": <select name='audio_i2s_slot'>"
        "<option value='left'" + String(cfg_audio_i2s_slot == "left" ? " selected" : "") + ">left</option>"
        "<option value='right'" + String(cfg_audio_i2s_slot == "right" ? " selected" : "") + ">right</option>"
        "<option value='stereo'" + String(cfg_audio_i2s_slot == "stereo" ? " selected" : "") + ">stereo</option>"
        "</select><br>"
        "</div>"
        "<small class='muted'>" + audioUiText(UI_AUDIO_SAVE_HARDWARE_NOTE) + "</small>"
        "</div>";

    if (audioCaps.available) {
        html +=
            "<p class='muted'>Backend: " +
            String((unsigned long)audioCaps.minSampleRate) + ".." +
            String((unsigned long)audioCaps.maxSampleRate) + " Hz; " +
            audioUiText(UI_AUDIO_RECOMMENDED) + " " +
            String((unsigned long)audioCaps.recommendedSampleRate) +
            " Hz.</p>";
    }

    html +=
        "<div style='margin-top:16px;text-align:right'>"
        "<button type='button' onclick=\"sfAudioAdvancedClose()\">" +
        audioUiText(UI_AUDIO_CLOSE) +
        "</button>"
        "</div></div></div>";

    html +=
        "<script>"
        "function sfAudioAdvancedOpen(){var m=document.getElementById('sfAudioAdvancedModal');if(m)m.style.display='block';}"
        "function sfAudioAdvancedClose(){var m=document.getElementById('sfAudioAdvancedModal');if(m)m.style.display='none';}"
        "function sfAudioUi(){"
        "var a=document.getElementById('cfgAudioEnabled');var r=document.getElementById('cfgRecordingFormat');var e=document.getElementById('cfgAudioExpertMode');var s=document.getElementById('cfgAudioSource');var p=document.getElementById('cfgAudioExpertPanel');var b=document.getElementById('cfgAudioBackend');var pp=document.getElementById('cfgAudioPdmPanel');var ip=document.getElementById('cfgAudioI2sPanel');"
        "if(a&&r&&a.value==='1')r.value='mkv';"
        "if(!e||!s||!p||!b||!pp||!ip)return;var expert=e.value==='1';if(!expert&&s.value==='external')s.value='board_default';p.style.display=expert?'block':'none';var external=expert&&s.value==='external';b.disabled=!external;pp.style.display=external&&b.value==='pdm'?'block':'none';ip.style.display=external&&b.value==='i2s'?'block':'none';}"
        "async function sfAudioMicTest(){"
        "var b=document.getElementById('sfAudioMicTestBtn'),st=document.getElementById('sfAudioMicTestStatus'),pb=document.getElementById('sfAudioMicTestPlayback'),au=document.getElementById('sfAudioMicTestAudio');"
        "if(!b||!st||!pb||!au)return;b.disabled=true;pb.style.display='none';au.pause();au.removeAttribute('src');"
        "st.textContent='" + audioUiText(UI_AUDIO_MIC_TEST_RECORDING) + "';"
        "try{var r=await fetch('/audio_test_record',{method:'POST',credentials:'same-origin'});var t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));var j=JSON.parse(t);au.src='/audio_test_play?t='+Date.now();var dl=document.getElementById('sfAudioMicTestDownload');if(dl)dl.href='/audio_test_play?download=1&t='+Date.now();pb.style.display='block';st.textContent='" + audioUiText(UI_AUDIO_MIC_TEST_DONE) + " '+(j.capture_ms||0)+' ms · Peak '+(j.peak||0)+' · RMS '+Number(j.rms||0).toFixed(1);}"
        "catch(e){st.textContent='" + audioUiText(UI_AUDIO_MIC_TEST_FAILED) + " '+e.message;}finally{b.disabled=false;}}"
        "sfAudioUi();"
        "</script>"
        "</div>";
}
