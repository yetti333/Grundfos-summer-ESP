#include "WifiManager.h"

#if __has_include("secrets.h")
#include "secrets.h"
#define HAS_SECRETS 1
#else
#define HAS_SECRETS 0
#endif

WifiManager::WifiManager(QueueHandle_t stateQ, ConfigStorage& storage)
    : _stateQ(stateQ), _storage(storage), _provisioning(false), _lastAttemptMs(0) {}

bool WifiManager::loadCredentials() {
#if HAS_SECRETS
    _ssid = WIFI_SSID;
    _pass = WIFI_PASSWORD;
    return !_ssid.isEmpty();
#else
    return _storage.loadWifi(_ssid, _pass);
#endif
}

void WifiManager::begin(bool forceProvision) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);

    if (forceProvision || !loadCredentials()) {
        startProvisionAp();
        StateEvent sev{StateEventType::WIFI_PROVISION_REQUIRED, 0, 0, 0, false};
        xQueueSend(_stateQ, &sev, 0);
        return;
    }
    connectIfNeeded();
}

void WifiManager::startProvisionAp() {
    _provisioning = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Grundfos-Provision", "grundfos123");
    MDNS.begin("grundfos-pump");
}

void WifiManager::connectIfNeeded() {
    if (_ssid.isEmpty()) return;
    if (WiFi.status() == WL_CONNECTED) return;
    uint32_t now = millis();
    if (now - _lastAttemptMs < 10000) return;
    _lastAttemptMs = now;
    WiFi.mode(WIFI_STA);
    WiFi.begin(_ssid.c_str(), _pass.c_str());
}

int32_t WifiManager::rssi() const {
    return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
}

bool WifiManager::connected() const {
    return WiFi.status() == WL_CONNECTED;
}

void WifiManager::taskLoop() {
    static wl_status_t last = WL_IDLE_STATUS;
    wl_status_t s = WiFi.status();

    if (!_provisioning) connectIfNeeded();

    if (s != last) {
        if (s == WL_CONNECTED) {
            StateEvent ev{StateEventType::WIFI_CONNECTED, (int32_t)WiFi.RSSI(), 0, 0, true};
            xQueueSend(_stateQ, &ev, 0);
        } else {
            StateEvent ev{StateEventType::WIFI_DISCONNECTED, 0, 0, 0, false};
            xQueueSend(_stateQ, &ev, 0);
        }
        last = s;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}