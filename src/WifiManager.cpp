#include "WifiManager.h"

#if __has_include("secrets.h")
#include "secrets.h"
#define HAS_SECRETS 1
#else
#define HAS_SECRETS 0
#endif

static const char* wifiDisconnectReasonToString(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_LEAVE: return "AUTH_LEAVE";
        case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
        case WIFI_REASON_ASSOC_TOOMANY: return "ASSOC_TOOMANY";
        case WIFI_REASON_NOT_AUTHED: return "NOT_AUTHED";
        case WIFI_REASON_NOT_ASSOCED: return "NOT_ASSOCED";
        case WIFI_REASON_ASSOC_LEAVE: return "ASSOC_LEAVE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: return "GROUP_KEY_UPDATE_TIMEOUT";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS: return "IE_IN_4WAY_DIFFERS";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        default: return "UNKNOWN";
    }
}

WifiManager::WifiManager(QueueHandle_t stateQ, ConfigStorage& storage)
    : _stateQ(stateQ), _storage(storage), _provisioning(false), _mdnsRunning(false), _lastAttemptMs(0) {
    // Register WiFi event handler for better error diagnostics
    WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
        this->onWiFiEvent(event, info);
    });
}

bool WifiManager::loadCredentials() {
#if HAS_SECRETS
    _ssid = WIFI_SSID;
    _pass = WIFI_PASSWORD;
    Serial.println("[CRED] Using WiFi credentials from secrets.h");
    Serial.printf("[CRED] SSID length: %d, first chars: %s...\n", _ssid.length(), _ssid.substring(0, 3).c_str());
    return !_ssid.isEmpty();
#else
    Serial.println("[CRED] Loading WiFi credentials from storage");
    bool ok = _storage.loadWifi(_ssid, _pass);
    if (ok) {
        Serial.printf("[CRED] Storage read OK: SSID='%s' (len=%d), PASS len=%d\n", 
                     _ssid.c_str(), _ssid.length(), _pass.length());
    } else {
        Serial.println("[CRED] Storage read FAILED: SSID is empty");
    }
    return ok;
#endif
}

void WifiManager::begin(bool forceProvision) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOSTNAME);
    // Keep reconnect strategy under our control in taskLoop/connectIfNeeded.
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
    Serial.println("[PROV] Starting WiFi provisioning AP");

    stopMDNS();

    // 1) AP+STA je praktictejsi: AP bezi, ale ESP se muze po provisioningu hned pripojit do STA.
    if (!WiFi.mode(WIFI_AP_STA)) {
        Serial.println("[PROV] ERROR: WiFi.mode(WIFI_AP_STA) failed");
    }

    // 2) Fixni AP IP musi sedet s Android appkou (u tebe 10.66.12.184).
    IPAddress apIp(10, 66, 12, 184);
    IPAddress gateway(10, 66, 12, 184);
    IPAddress subnet(255, 255, 255, 0);

    if (!WiFi.softAPConfig(apIp, gateway, subnet)) {
        Serial.println("[PROV] ERROR: softAPConfig failed");
    }

    bool apOk = WiFi.softAP("Grundfos-Provision", "grundfos123");
    if (!apOk) {
        Serial.println("[PROV] ERROR: softAP start failed");
        return;
    }

    Serial.print("[PROV] AP IP: ");
    Serial.println(WiFi.softAPIP());

    // 3) mDNS hostname pro klienta; kdyz selze, provisioning ma stale fungovat pres IP.
    startMDNS();

    Serial.println("[PROV] AP ready");
}

void WifiManager::connectIfNeeded() {
    if (_ssid.isEmpty()) return;
    if (WiFi.status() == WL_CONNECTED) return;
    uint32_t now = millis();
    if (now - _lastAttemptMs < 10000) return;
    _lastAttemptMs = now;

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOSTNAME);
    WiFi.begin(_ssid.c_str(), _pass.c_str());

    Serial.printf("Attempting to connect to WiFi SSID '%s'\n", _ssid.c_str());
}

void WifiManager::exitProvisioning() {
    if (!_provisioning) return;
    _provisioning = false;
    Serial.println("[PROV] Exiting provisioning mode");
    WiFi.softAPdisconnect(true);
    stopMDNS();
    connectIfNeeded();
}

void WifiManager::startMDNS() {
    MDNS.end();

    if (!MDNS.begin(MDNS_HOSTNAME)) {
        _mdnsRunning = false;
        Serial.println("[mDNS] CHYBA: nepodarilo se spustit");
        return;
    }

    MDNS.addService("http", "tcp", 80);
    _mdnsRunning = true;
    Serial.println(String("[mDNS] OK: ") + MDNS_HOSTNAME + ".local");
}

void WifiManager::stopMDNS() {
    if (!_mdnsRunning) {
        return;
    }

    MDNS.end();
    _mdnsRunning = false;
    Serial.println("[mDNS] Stopped");
}

int32_t WifiManager::rssi() const {
    return (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -127;
}

bool WifiManager::connected() const {
    return WiFi.status() == WL_CONNECTED;
}

bool WifiManager::provisioningActive() const {
    return _provisioning;
}

bool WifiManager::mdnsRunning() const {
    return _mdnsRunning;
}

const char* WifiManager::hostname() const {
    return MDNS_HOSTNAME;
}

const char* WifiManager::mdnsHostname() const {
    return MDNS_HOSTNAME;
}

String WifiManager::mdnsUrl() const {
    return String("http://") + MDNS_HOSTNAME + ".local/";
}

void WifiManager::onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            uint8_t reason = info.wifi_sta_disconnected.reason;
            Serial.printf("WiFi disconnected, reason: %u (%s)\n", reason, wifiDisconnectReasonToString(reason));
            Serial.println("[WiFi] Odpojeno, cekam na reconnect...");
            stopMDNS();

            if (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                Serial.println("WiFi Error: 4WAY_HANDSHAKE_TIMEOUT - Possible causes:");
                Serial.println("  - Incorrect password");
                Serial.println("  - Wrong security protocol (check if AP uses WPA3 only)");
                Serial.println("  - Weak signal strength");
                Serial.println("  - AP rejecting connection (MAC filter, connection limit)");
                Serial.printf("  - Current RSSI: %d dBm\n", WiFi.RSSI());
            } else if (reason == WIFI_REASON_AUTH_LEAVE) {
                Serial.println("WiFi AUTH_LEAVE: AP ukoncil autentizaci klienta.");
                Serial.println("  - Zkontroluj, ze AP nepouziva WPA3-only");
                Serial.println("  - Zkontroluj limit klientu nebo MAC filtering");
                Serial.println("  - Otestuj stejne SSID/heslo s telefonem ve stejnem miste");
            }

            // Keep a short backoff to avoid reconnect storms when AP keeps rejecting us.
            _lastAttemptMs = millis();
            break;
        }

        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("WiFi connected to AP");
            break;

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[WiFi] Nova IP: %s\n", WiFi.localIP().toString().c_str());
            startMDNS();
            break;

        default:
            break;
    }
}

void WifiManager::taskLoop() {
    static wl_status_t last = WL_IDLE_STATUS;
    wl_status_t s = WiFi.status();

    // Check if we can exit provisioning mode
    if (_provisioning && loadCredentials()) {
        exitProvisioning();
    }

    if (!_provisioning) connectIfNeeded();

    if (s != last) {
        switch (s) {
            case WL_CONNECTED: {
                StateEvent ev{StateEventType::WIFI_CONNECTED, (int32_t)WiFi.RSSI(), 0, 0, true};
                Serial.printf("Connected to WiFi SSID '%s', RSSI: %d dBm\n", WiFi.SSID().c_str(), WiFi.RSSI());
                Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
                xQueueSend(_stateQ, &ev, 0);
                break;
            }

            case WL_CONNECT_FAILED: {
                Serial.println("WiFi connection failed - check credentials and signal strength");
                StateEvent ev{StateEventType::WIFI_DISCONNECTED, 0, 0, 0, false};
                xQueueSend(_stateQ, &ev, 0);
                break;
            }

            case WL_CONNECTION_LOST: {
                Serial.println("WiFi connection lost");
                StateEvent ev{StateEventType::WIFI_DISCONNECTED, 0, 0, 0, false};
                xQueueSend(_stateQ, &ev, 0);
                break;
            }

            case WL_DISCONNECTED: {
                // Only log if we were previously connected to avoid spam
                if (last == WL_CONNECTED) {
                    Serial.println("WiFi disconnected");
                    StateEvent ev{StateEventType::WIFI_DISCONNECTED, 0, 0, 0, false};
                    xQueueSend(_stateQ, &ev, 0);
                }
                break;
            }

            default: {
                if (last == WL_CONNECTED) {
                    Serial.printf("WiFi status changed to: %d\n", s);
                    StateEvent ev{StateEventType::WIFI_DISCONNECTED, 0, 0, 0, false};
                    xQueueSend(_stateQ, &ev, 0);
                }
                break;
            }
        }
        last = s;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}
