#include "wifi_link_diag.h"
#include "logger.h"

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>

namespace WifiLinkDiag {
namespace {

enum class Kind : uint8_t { Connected, Disconnected, GotIp, LostIp };

struct Event {
    Kind kind;
    uint8_t reason;     // Disconnected: 802.11 reason code
    int8_t rssi;        // Disconnected: RSSI at disconnect
    uint8_t channel;    // Connected: AP channel
    uint32_t ip;        // GotIp: address (network byte order)
    uint32_t atMs;
};

// The WiFi event handler runs in the Arduino event task. It only queues a
// small POD record; all String/console work happens in service() on loop().
constexpr size_t kQueueSize = 16;
Event queue[kQueueSize];
size_t head = 0;
size_t count = 0;
uint32_t dropped = 0;
portMUX_TYPE queueMux = portMUX_INITIALIZER_UNLOCKED;
bool registered = false;

// Loop-side statistics.
uint32_t disconnects = 0;
uint32_t lastDisconnectMs = 0;
bool linkDown = false;

void push(const Event &e) {
    portENTER_CRITICAL(&queueMux);
    if (count < kQueueSize) {
        queue[(head + count) % kQueueSize] = e;
        ++count;
    } else {
        ++dropped;
    }
    portEXIT_CRITICAL(&queueMux);
}

bool pop(Event &e, uint32_t &droppedOut) {
    bool ok = false;
    portENTER_CRITICAL(&queueMux);
    droppedOut = dropped;
    dropped = 0;
    if (count) {
        e = queue[head];
        head = (head + 1) % kQueueSize;
        --count;
        ok = true;
    }
    portEXIT_CRITICAL(&queueMux);
    return ok;
}

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    Event e = {};
    e.atMs = millis();
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        e.kind = Kind::Connected;
        e.channel = info.wifi_sta_connected.channel;
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        e.kind = Kind::Disconnected;
        e.reason = info.wifi_sta_disconnected.reason;
        e.rssi = info.wifi_sta_disconnected.rssi;
        break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        e.kind = Kind::GotIp;
        e.ip = info.got_ip.ip_info.ip.addr;
        break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        e.kind = Kind::LostIp;
        break;
    default:
        return;
    }
    push(e);
}

}  // namespace

void begin() {
    if (registered)
        return;
    registered = true;
    WiFi.onEvent(onWifiEvent);
}

void service() {
    Event e;
    uint32_t droppedNow = 0;
    while (pop(e, droppedNow)) {
        if (droppedNow)
            consoleWrite("WIFI-DIAG", "event queue overflow | dropped=" + String((unsigned long)droppedNow));
        switch (e.kind) {
        case Kind::Connected:
            consoleWrite("WIFI-DIAG", "STA associated | channel=" + String(e.channel) +
                                      " | at_ms=" + String((unsigned long)e.atMs));
            break;
        case Kind::Disconnected:
            ++disconnects;
            lastDisconnectMs = e.atMs;
            linkDown = true;
            consoleWrite("WIFI-DIAG", "STA disconnected | reason=" + String(e.reason) + " (" +
                                      String(WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(e.reason))) +
                                      ") | rssi=" + String(e.rssi) + " dBm | count=" +
                                      String((unsigned long)disconnects) +
                                      " | at_ms=" + String((unsigned long)e.atMs));
            break;
        case Kind::GotIp: {
            String text = "STA got IP | ip=" + IPAddress(e.ip).toString() +
                          " | rssi=" + String(WiFi.RSSI()) + " dBm";
            if (linkDown)
                text += " | down_ms=" + String((unsigned long)(e.atMs - lastDisconnectMs));
            text += " | at_ms=" + String((unsigned long)e.atMs);
            linkDown = false;
            consoleWrite("WIFI-DIAG", text);
            break;
        }
        case Kind::LostIp:
            consoleWrite("WIFI-DIAG", "STA lost IP | at_ms=" + String((unsigned long)e.atMs));
            break;
        }
    }
    if (droppedNow)
        consoleWrite("WIFI-DIAG", "event queue overflow | dropped=" + String((unsigned long)droppedNow));
}

}  // namespace WifiLinkDiag
