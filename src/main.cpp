#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include "secrets.h"   // z include/secrets.h

// =========================
// WiFi + NTP configuration
// =========================
static const char *DEVICE_HOSTNAME = "ESP32";

static const char *NTP_SERVER_1 = "pool.ntp.org";
static const char *NTP_SERVER_2 = "time.google.com";
static const long GMT_OFFSET_SEC = 3600;      // CET
static const int DAYLIGHT_OFFSET_SEC = 3600;  // CEST

// =========================
// Hardware mapping
// =========================
static constexpr uint8_t PIN_LED_GREEN = 21;
static constexpr uint8_t PIN_LED_RED = 18;
static constexpr uint8_t PIN_LED_BLUE = 19;
static constexpr uint8_t PIN_BUTTON = 14;   // INPUT_PULLUP, active LOW
static constexpr uint8_t PIN_RELAY = 13;
static constexpr uint8_t PIN_FEEDBACK = 27; // pump pulse input

// =========================
// Timing constants
// =========================
static constexpr uint32_t BLINK_UNIT_MS = 100;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static constexpr uint32_t NTP_SYNC_TIMEOUT_MS = 20000;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 40;
static constexpr uint32_t BUTTON_SHORT_MAX_MS = 1000;
static constexpr uint32_t BUTTON_LONG_MIN_MS = 3000;
static constexpr uint32_t FEEDBACK_WINDOW_MS = 60000;               // 1 minute
static constexpr uint32_t AUTO_RUN_TOTAL_MS = 5UL * 60UL * 1000UL;  // 5 minutes
static constexpr uint32_t BYPASS_START_LED_MS = 1000;
static constexpr uint32_t BLE_RESTART_DELAY_MS = 1200;

// ~75 Hz = ~4500 pulses per minute, threshold with margin
static constexpr uint32_t MIN_VALID_PULSES_PER_MIN = 500;

// =========================
// BLE provisioning UUIDs
// =========================
static const char *BLE_SERVICE_UUID = "00001234-0000-1000-8000-00805f9b34fb";
static const char *BLE_CHAR_SSID_UUID = "00001235-0000-1000-8000-00805f9b34fb";
static const char *BLE_CHAR_PASS_UUID = "00001236-0000-1000-8000-00805f9b34fb";
static const char *BLE_CHAR_CONFIRM_UUID = "00001237-0000-1000-8000-00805f9b34fb";
static const char *BLE_CHAR_IP_UUID = "00001238-0000-1000-8000-00805f9b34fb";

// =========================
// NVS keys
// =========================
static const char *NVS_NS = "grundfos";
static const char *NVS_WIFI_SSID = "wifi_ssid";
static const char *NVS_WIFI_PASS = "wifi_pass";
static const char *NVS_START_HOUR = "start_h";
static const char *NVS_START_MINUTE = "start_m";
static const char *NVS_RUN_MINUTES = "run_min";
static const char *NVS_FB_TIMEOUT_S = "fb_to_s";

// =========================
// Debug logging
// =========================
static bool g_debug = true;

#define LOGI(fmt, ...) do { if (g_debug) Serial.printf("[INFO] " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOGW(fmt, ...) do { if (g_debug) Serial.printf("[WARN] " fmt "\n", ##__VA_ARGS__); } while (0)
#define LOGE(fmt, ...) do { if (g_debug) Serial.printf("[ERR ] " fmt "\n", ##__VA_ARGS__); } while (0)

// =========================
// State machine
// =========================
enum class SystemState : uint8_t {
    BLE_PROVISIONING,
    WIFI_CONNECTING,
    NTP_SYNCING,
    AUTO_MODE,
    MANUAL_MODE,
    ERROR_COMM,
    ERROR_TIME,
    ERROR_PUMP
};

struct ErrorFlags {
    bool comm = false;
    bool timeSync = false;
    bool pumpRun = false;
};

SystemState g_state = SystemState::WIFI_CONNECTING;
SystemState g_stateBeforePumpError = SystemState::AUTO_MODE;
ErrorFlags g_errors;

// =========================
// Runtime variables
// =========================
bool g_bypass = false;
bool g_relayOn = false;
bool g_feedbackOk = false;
bool g_feedbackWindowActive = false;
bool g_autoRunActive = false; // true only for scheduled 5-minute automatic run

bool g_bleProvisioningActive = false;
bool g_bleRestartRequested = false;
bool g_bleIpNotified = false;
uint32_t g_bleRestartAtMs = 0;

uint8_t g_startHour = 19;
uint8_t g_startMinute = 0;
uint32_t g_runMinutes = 5;
uint32_t g_feedbackTimeoutSec = 60;
uint32_t g_autoRunTotalMsCfg = AUTO_RUN_TOTAL_MS;
uint32_t g_feedbackWindowMsCfg = FEEDBACK_WINDOW_MS;

Preferences g_prefs;
String g_wifiSsid;
String g_wifiPass;
String g_pendingBleSsid;
String g_pendingBlePass;

uint32_t g_bootMs = 0;
uint32_t g_wifiStartMs = 0;
uint32_t g_ntpStartMs = 0;
uint32_t g_relayStartMs = 0;
uint32_t g_feedbackWindowStartMs = 0;

int g_lastScheduledYday = -1;

// =========================
// Pulse ISR data
// =========================
volatile uint32_t g_pulseCounter = 0;
portMUX_TYPE g_pulseMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_feedbackWindowStartPulse = 0;

AsyncWebServer g_server(80);
bool g_serverStarted = false;
NimBLEServer *g_bleServer = nullptr;
NimBLECharacteristic *g_bleSsidChar = nullptr;
NimBLECharacteristic *g_blePassChar = nullptr;
NimBLECharacteristic *g_bleConfirmChar = nullptr;
NimBLECharacteristic *g_bleIpChar = nullptr;

// =========================
// Button debouncing
// =========================
struct ButtonState {
    bool rawLevel = HIGH;
    bool stableLevel = HIGH;
    bool lastStableLevel = HIGH;
    uint32_t lastChangeMs = 0;
    uint32_t pressedSinceMs = 0;
} g_btn;

bool g_shortPressEvent = false;
bool g_longPressEvent = false;

// =========================
// Forward declarations
// =========================
void handleWifi();
void handleNTP();
void handleButton();
void handleRelay();
void handleFeedback();
void handleLED();
void handleErrors();
void schedulePump();
void handleProvisioning();

uint32_t readPulseCount();
bool blinkRatio(uint8_t onUnits, uint8_t offUnits);
void setLeds(bool g, bool r, bool b);
void startRelay(bool autoRun);
void stopRelay();
bool getLocalTimeSafe(struct tm &outTm);
void resetCommErrorAndRetry();
void resetTimeErrorAndRetry();
void clearPumpError();
const char *stateName(SystemState s);
void setState(SystemState next);
void loadWifiFromNvs();
void loadRuntimeSettingsFromNvs();
void saveWifiToNvs(const String &ssid, const String &pass);
void saveRuntimeSettingsToNvs(uint8_t startHour, uint8_t startMinute, uint32_t runMinutes, uint32_t feedbackTimeoutSec);
void applyRuntimeSettings(uint8_t startHour, uint8_t startMinute, uint32_t runMinutes, uint32_t feedbackTimeoutSec);
void setupBleProvisioning();
void setupRestApi();
void setupWifiEventLogging();
void logVisibleNetworks();
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void notifyBleIpAddress();
bool parseStartTime(const String &value, uint8_t &hourOut, uint8_t &minuteOut);
bool hasAnyErrorActive();

// =========================
// ISR
// =========================
void IRAM_ATTR onFeedbackPulse() {
    portENTER_CRITICAL_ISR(&g_pulseMux);
    g_pulseCounter++;
    portEXIT_CRITICAL_ISR(&g_pulseMux);
}

uint32_t readPulseCount() {
    uint32_t v;
    portENTER_CRITICAL(&g_pulseMux);
    v = g_pulseCounter;
    portEXIT_CRITICAL(&g_pulseMux);
    return v;
}

// =========================
// Helpers
// =========================
const char *stateName(SystemState s) {
    switch (s) {
        case SystemState::BLE_PROVISIONING: return "BLE_PROVISIONING";
        case SystemState::WIFI_CONNECTING: return "WIFI_CONNECTING";
        case SystemState::NTP_SYNCING: return "NTP_SYNCING";
        case SystemState::AUTO_MODE: return "AUTO_MODE";
        case SystemState::MANUAL_MODE: return "MANUAL_MODE";
        case SystemState::ERROR_COMM: return "ERROR_COMM";
        case SystemState::ERROR_TIME: return "ERROR_TIME";
        case SystemState::ERROR_PUMP: return "ERROR_PUMP";
        default: return "UNKNOWN";
    }
}

void setState(SystemState next) {
    if (g_state != next) {
        LOGI("STATE %s -> %s", stateName(g_state), stateName(next));
        g_state = next;
    }
}

void applyRuntimeSettings(uint8_t startHour, uint8_t startMinute, uint32_t runMinutes, uint32_t feedbackTimeoutSec) {
g_startHour = startHour;
g_startMinute = startMinute;
g_runMinutes = runMinutes;
g_feedbackTimeoutSec = feedbackTimeoutSec;
g_autoRunTotalMsCfg = g_runMinutes * 60UL * 1000UL;
g_feedbackWindowMsCfg = g_feedbackTimeoutSec * 1000UL;
}

void loadRuntimeSettingsFromNvs() {
    uint8_t startHour = g_prefs.getUChar(NVS_START_HOUR, 19);
    uint8_t startMinute = g_prefs.getUChar(NVS_START_MINUTE, 0);
    uint32_t runMinutes = g_prefs.getUInt(NVS_RUN_MINUTES, 5);
    uint32_t feedbackTimeoutSec = g_prefs.getUInt(NVS_FB_TIMEOUT_S, 60);

    if (startHour > 23) startHour = 19;
    if (startMinute > 59) startMinute = 0;
    if (runMinutes == 0) runMinutes = 5;
    if (feedbackTimeoutSec == 0) feedbackTimeoutSec = 60;

    applyRuntimeSettings(startHour, startMinute, runMinutes, feedbackTimeoutSec);
}

void saveRuntimeSettingsToNvs(uint8_t startHour, uint8_t startMinute, uint32_t runMinutes, uint32_t feedbackTimeoutSec) {
g_prefs.putUChar(NVS_START_HOUR, startHour);
g_prefs.putUChar(NVS_START_MINUTE, startMinute);
g_prefs.putUInt(NVS_RUN_MINUTES, runMinutes);
g_prefs.putUInt(NVS_FB_TIMEOUT_S, feedbackTimeoutSec);
}

void loadWifiFromNvs() {
    g_wifiSsid = g_prefs.getString(NVS_WIFI_SSID, "");
    g_wifiPass = g_prefs.getString(NVS_WIFI_PASS, "");
    LOGI("Loaded WiFi credentials from NVS: SSID='%s' PASS='%s'", g_wifiSsid.c_str(), g_wifiPass.c_str());

}

void saveWifiToNvs(const String &ssid, const String &pass) {
    g_prefs.putString(NVS_WIFI_SSID, ssid);
    g_prefs.putString(NVS_WIFI_PASS, pass);
}

bool parseStartTime(const String &value, uint8_t &hourOut, uint8_t &minuteOut) {
    if (value.length() != 5 || value.charAt(2) != ':') return false;

    int h = value.substring(0, 2).toInt();
    int m = value.substring(3, 5).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;

    hourOut = static_cast<uint8_t>(h);
    minuteOut = static_cast<uint8_t>(m);
    return true;
}

bool hasAnyErrorActive() {
    return g_errors.comm || g_errors.timeSync || g_errors.pumpRun;
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            LOGI("WiFi event: STA_START");
            break;

        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            LOGI("WiFi event: STA_CONNECTED");
            break;

        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            LOGI("WiFi event: GOT_IP ip=%s", WiFi.localIP().toString().c_str());
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            LOGW("WiFi event: STA_DISCONNECTED reason=%d", info.wifi_sta_disconnected.reason);
            break;

        default:
            break;
    }
}

void setupWifiEventLogging() {
    WiFi.onEvent(onWiFiEvent);
}

void logVisibleNetworks() {
    int networkCount = WiFi.scanNetworks();

    if (networkCount < 0) {
        LOGW("WiFi scan failed: %d", networkCount);
        return;
    }

    LOGI("WiFi scan found %d network(s)", networkCount);

    for (int index = 0; index < networkCount; ++index) {
        LOGI("WiFi scan[%d]: SSID='%s' RSSI=%d CH=%d ENC=%d",
             index,
             WiFi.SSID(index).c_str(),
             WiFi.RSSI(index),
             WiFi.channel(index),
             WiFi.encryptionType(index));
    }

    WiFi.scanDelete();
}

class ProvisioningServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer *) override {
        LOGI("BLE client connected");
    }

    void onDisconnect(NimBLEServer *server) override {
        LOGI("BLE client disconnected");
        (void)server;
        NimBLEDevice::startAdvertising();
    }
};

class ProvisioningCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic *characteristic) override {
        if (characteristic == nullptr) return;

        std::string raw = characteristic->getValue();
        String value(raw.c_str());

        if (characteristic == g_bleSsidChar) {
            g_pendingBleSsid = value;
            LOGI("BLE provisioning SSID written: '%s'", g_pendingBleSsid.c_str());
            return;
        }

        if (characteristic == g_blePassChar) {
            g_pendingBlePass = value;
            LOGI("BLE provisioning PASSWORD written: '%s'", g_pendingBlePass.c_str());
            return;
        }

        if (characteristic == g_bleConfirmChar) {
            LOGI("BLE provisioning CONFIRM written");
            if (g_pendingBleSsid.length() == 0) {
                LOGW("BLE provisioning missing SSID");
                return;
            }

            saveWifiToNvs(g_pendingBleSsid, g_pendingBlePass);
            g_wifiSsid = g_pendingBleSsid;
            g_wifiPass = g_pendingBlePass;
            g_bleRestartRequested = true;
            g_bleRestartAtMs = millis() + BLE_RESTART_DELAY_MS;
            LOGI("BLE provisioning saved, restart requested in %lu ms", (unsigned long)BLE_RESTART_DELAY_MS);
        }
    }
};

void setupBleProvisioning() {
    uint64_t chipId = ESP.getEfuseMac();
    char bleName[24];
    snprintf(bleName, sizeof(bleName), "ESP32-%04X", (uint16_t)(chipId & 0xFFFF));

    NimBLEDevice::init(bleName);
    LOGI("BLE advertising name: %s", bleName);
    g_bleServer = NimBLEDevice::createServer();
    g_bleServer->setCallbacks(new ProvisioningServerCallbacks());

    NimBLEService *service = g_bleServer->createService(BLE_SERVICE_UUID);
    g_bleSsidChar = service->createCharacteristic(BLE_CHAR_SSID_UUID, NIMBLE_PROPERTY::WRITE);
    g_blePassChar = service->createCharacteristic(BLE_CHAR_PASS_UUID, NIMBLE_PROPERTY::WRITE);
    g_bleConfirmChar = service->createCharacteristic(BLE_CHAR_CONFIRM_UUID, NIMBLE_PROPERTY::WRITE);
    g_bleIpChar = service->createCharacteristic(BLE_CHAR_IP_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    auto *callbacks = new ProvisioningCharacteristicCallbacks();
    g_bleSsidChar->setCallbacks(callbacks);
    g_blePassChar->setCallbacks(callbacks);
    g_bleConfirmChar->setCallbacks(callbacks);
    g_bleIpChar->setValue("");

    service->start();
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->start();
    LOGI("BLE provisioning service started");
}

void notifyBleIpAddress() {
    if (g_bleIpChar == nullptr) return;

    String ip = WiFi.localIP().toString();
    if (ip.length() == 0) return;

    g_bleIpChar->setValue(ip.c_str());
    g_bleIpChar->notify();
    g_bleIpNotified = true;
    LOGI("BLE IP notify: %s", ip.c_str());
}

void setupRestApi() {
    g_server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        doc["mode"] = (g_state == SystemState::MANUAL_MODE) ? "MANUAL" : "AUTO";
        doc["pumpRunning"] = g_relayOn;
        doc["feedback"] = readPulseCount();
        doc["feedbackStable"] = g_feedbackOk;
        doc["bypass"] = g_bypass;

        JsonObject errors = doc.createNestedObject("errors");
        errors["wifi"] = g_errors.comm;
        errors["time"] = g_errors.timeSync;
        errors["pump"] = g_errors.pumpRun;

        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
    });

    auto *modeHandler = new AsyncCallbackJsonWebHandler("/mode", [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!json.is<JsonObject>()) {
            request->send(400, "application/json", "{\"error\":\"invalid_json\"}");
            return;
        }

        JsonObject obj = json.as<JsonObject>();
        const char *mode = obj["mode"] | "";

        if (strcmp(mode, "AUTO") != 0 && strcmp(mode, "MANUAL") != 0) {
            request->send(400, "application/json", "{\"error\":\"mode_must_be_AUTO_or_MANUAL\"}");
            return;
        }

        if (hasAnyErrorActive()) {
            request->send(409, "application/json", "{\"error\":\"state_in_error\"}");
            return;
        }

        if (strcmp(mode, "MANUAL") == 0 && g_state == SystemState::AUTO_MODE) {
            setState(SystemState::MANUAL_MODE);
        } else if (strcmp(mode, "AUTO") == 0 && g_state == SystemState::MANUAL_MODE) {
            stopRelay();
            setState(SystemState::AUTO_MODE);
        }

        request->send(200, "application/json", "{\"ok\":true}");
    });
    g_server.addHandler(modeHandler);

    auto *pumpStartHandler = new AsyncCallbackJsonWebHandler("/pump/start", [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!json.is<JsonObject>()) {
            request->send(400, "application/json", "{\"error\":\"invalid_json\"}");
            return;
        }

        if (hasAnyErrorActive()) {
            request->send(409, "application/json", "{\"error\":\"state_in_error\"}");
            return;
        }

        if (g_state != SystemState::MANUAL_MODE) {
            request->send(409, "application/json", "{\"error\":\"manual_mode_required\"}");
            return;
        }

        JsonObject obj = json.as<JsonObject>();
        if (obj.containsKey("bypass")) {
            g_bypass = obj["bypass"].as<bool>();
        }

        startRelay(false);
        request->send(200, "application/json", "{\"ok\":true}");
    });
    g_server.addHandler(pumpStartHandler);

    g_server.on("/pump/stop", HTTP_POST, [](AsyncWebServerRequest *request) {
        stopRelay();
        request->send(200, "application/json", "{\"ok\":true}");
    });

    auto *settingsHandler = new AsyncCallbackJsonWebHandler("/settings", [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!json.is<JsonObject>()) {
            request->send(400, "application/json", "{\"error\":\"invalid_json\"}");
            return;
        }

        JsonObject obj = json.as<JsonObject>();
        const char *startTime = obj["startTime"] | "";
        uint32_t runMinutes = obj["runMinutes"] | 0;
        uint32_t feedbackTimeout = obj["feedbackTimeout"] | 0;

        uint8_t hour = 0;
        uint8_t minute = 0;
        if (!parseStartTime(String(startTime), hour, minute)) {
            request->send(400, "application/json", "{\"error\":\"startTime_format_HH:MM\"}");
            return;
        }

        if (runMinutes == 0 || feedbackTimeout == 0) {
            request->send(400, "application/json", "{\"error\":\"runMinutes_and_feedbackTimeout_must_be_gt_0\"}");
            return;
        }

        applyRuntimeSettings(hour, minute, runMinutes, feedbackTimeout);
        saveRuntimeSettingsToNvs(hour, minute, runMinutes, feedbackTimeout);

        request->send(200, "application/json", "{\"ok\":true}");
    });
    g_server.addHandler(settingsHandler);
    LOGI("REST API routes registered");
}

void handleProvisioning() {
    if (g_bleRestartRequested && (int32_t)(millis() - g_bleRestartAtMs) >= 0) {
        ESP.restart();
    }
}

bool blinkRatio(uint8_t onUnits, uint8_t offUnits) {
uint32_t cycle = (uint32_t)(onUnits + offUnits) * BLINK_UNIT_MS;
if (cycle == 0) return false;
uint32_t pos = millis() % cycle;
return pos < ((uint32_t)onUnits * BLINK_UNIT_MS);
}

void setLeds(bool g, bool r, bool b) {
    digitalWrite(PIN_LED_GREEN, g ? HIGH : LOW);
    digitalWrite(PIN_LED_RED, r ? HIGH : LOW);
    digitalWrite(PIN_LED_BLUE, b ? HIGH : LOW);
}

bool getLocalTimeSafe(struct tm &outTm) {
    time_t now = time(nullptr);
    if (now < 1700000000) return false;
    localtime_r(&now, &outTm);
    return true;
}

void startRelay(bool autoRun) {
    if (g_relayOn) return;

    g_relayOn = true;
    g_autoRunActive = autoRun;
    g_relayStartMs = millis();
    digitalWrite(PIN_RELAY, HIGH);

    g_feedbackWindowActive = true;
    g_feedbackWindowStartMs = millis();
    g_feedbackWindowStartPulse = readPulseCount();
    g_feedbackOk = g_bypass;

    LOGI("Relay ON (%s mode)", autoRun ? "AUTO schedule" : "MANUAL");
    if (g_bypass) LOGW("Bypass active: feedback ignored");
}

void stopRelay() {
    if (!g_relayOn) return;

    g_relayOn = false;
    g_autoRunActive = false;
    g_feedbackWindowActive = false;
    digitalWrite(PIN_RELAY, LOW);
    LOGI("Relay OFF");
}

void resetCommErrorAndRetry() {
    LOGI("Reset COMM error -> retry WiFi");
    g_errors.comm = false;
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(DEVICE_HOSTNAME);
    logVisibleNetworks();
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
    g_wifiStartMs = millis();
    setState(SystemState::WIFI_CONNECTING);
}

void resetTimeErrorAndRetry() {
    LOGI("Reset TIME error -> retry NTP");
    g_errors.timeSync = false;
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
    g_ntpStartMs = millis();
    setState(SystemState::NTP_SYNCING);
    logVisibleNetworks();
}

void clearPumpError() {
    LOGI("Reset PUMP error");
    g_errors.pumpRun = false;
    stopRelay();
    setState(g_stateBeforePumpError);
}

// =========================
// Required handlers
// =========================
void handleWifi() {
    if (g_state == SystemState::BLE_PROVISIONING) return;

    if (g_state == SystemState::WIFI_CONNECTING) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            LOGI("WiFi connected. IP=%s", WiFi.localIP().toString().c_str());
            notifyBleIpAddress();
            if (!g_serverStarted) {
                g_server.begin();
                g_serverStarted = true;
                LOGI("REST API server started on port 80");
            }
            configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
            g_ntpStartMs = millis();
            setState(SystemState::NTP_SYNCING);
            return;
        }

        if (millis() - g_wifiStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
            LOGE("WiFi connection timeout");
            g_errors.comm = true;
        }
    }

    // Runtime communication supervision
    if (g_state != SystemState::WIFI_CONNECTING && WiFi.status() != WL_CONNECTED) {
        if (!g_errors.comm) LOGE("WiFi lost");
        g_errors.comm = true;
    } else if (WiFi.status() == WL_CONNECTED && !g_bleIpNotified) {
        notifyBleIpAddress();
    }
}

void handleNTP() {
    if (g_state != SystemState::NTP_SYNCING) return;

    time_t now = time(nullptr);
    if (now >= 1700000000) {
        struct tm t;
        localtime_r(&now, &t);
        LOGI("NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        setState(SystemState::AUTO_MODE);
        return;
    }

    if (millis() - g_ntpStartMs >= NTP_SYNC_TIMEOUT_MS) {
        LOGE("NTP sync timeout");
        g_errors.timeSync = true;
    }
}

void handleButton() {
    uint32_t now = millis();
    bool raw = digitalRead(PIN_BUTTON);

    if (raw != g_btn.rawLevel) {
        g_btn.rawLevel = raw;
        g_btn.lastChangeMs = now;
    }

    if ((now - g_btn.lastChangeMs) >= BUTTON_DEBOUNCE_MS && g_btn.stableLevel != g_btn.rawLevel) {
        g_btn.lastStableLevel = g_btn.stableLevel;
        g_btn.stableLevel = g_btn.rawLevel;

        if (g_btn.stableLevel == LOW) {
            g_btn.pressedSinceMs = now;
        }

        if (g_btn.lastStableLevel == LOW && g_btn.stableLevel == HIGH) {
            uint32_t pressMs = now - g_btn.pressedSinceMs;
            if (pressMs < BUTTON_SHORT_MAX_MS) {
                g_shortPressEvent = true;
            }
            if (pressMs >= BUTTON_LONG_MIN_MS) {
                g_longPressEvent = true;
            }
            LOGI("Button release: %lu ms", (unsigned long)pressMs);
        }
    }

    if (g_shortPressEvent) {
        g_shortPressEvent = false;

        if (g_state == SystemState::ERROR_COMM) {
            resetCommErrorAndRetry();
            return;
        }

        if (g_state == SystemState::ERROR_TIME) {
            resetTimeErrorAndRetry();
            return;
        }

        if (g_state == SystemState::ERROR_PUMP) {
            clearPumpError();
            return;
        }

        if (g_state == SystemState::MANUAL_MODE) {
            LOGI("Manual short press: relay toggle");
            if (g_relayOn) stopRelay();
            else startRelay(false);
        }
    }

    if (g_longPressEvent) {
        g_longPressEvent = false;

        if (g_state == SystemState::AUTO_MODE) {
            LOGI("Long press: AUTO -> MANUAL");
            setState(SystemState::MANUAL_MODE);
            return;
        }

        if (g_state == SystemState::MANUAL_MODE) {
            LOGI("Long press: MANUAL -> AUTO");
            stopRelay();
            setState(SystemState::AUTO_MODE);
            return;
        }
    }
}

void schedulePump() {
    if (g_state != SystemState::AUTO_MODE) return;
    if (g_relayOn) return;

    struct tm t;
    if (!getLocalTimeSafe(t)) return;

    // Start daily once per day at configured time.
    if (t.tm_hour == g_startHour && t.tm_min == g_startMinute) {
        if (g_lastScheduledYday != t.tm_yday) {
            g_lastScheduledYday = t.tm_yday;
            LOGI("Schedule hit %02u:%02u -> start auto run", g_startHour, g_startMinute);
            startRelay(true);
        }
    }
}

void handleFeedback() {
    if (!g_relayOn) return;
    if (g_bypass) return;
    if (!g_feedbackWindowActive) return;

    uint32_t now = millis();
    if (now - g_feedbackWindowStartMs < g_feedbackWindowMsCfg) return;

    uint32_t pulseNow = readPulseCount();
    uint32_t delta = pulseNow - g_feedbackWindowStartPulse;
    LOGI("Feedback window done: pulses=%lu", (unsigned long)delta);

    if (delta >= MIN_VALID_PULSES_PER_MIN) {
        g_feedbackOk = true;
        LOGI("Feedback OK");
    } else {
        LOGE("Feedback FAIL -> pump error");
        g_errors.pumpRun = true;
        stopRelay();
        return;
    }

    g_feedbackWindowStartMs = now;
    g_feedbackWindowStartPulse = pulseNow;
}

void handleRelay() {
    if (!g_relayOn) return;

    if (g_autoRunActive) {
        if (millis() - g_relayStartMs >= g_autoRunTotalMsCfg) {
            LOGI("Auto run %lu min completed", (unsigned long)g_runMinutes);
            stopRelay();
        }
    }
    // Manual run stays ON until short press.
}

void handleErrors() {
    if (g_errors.pumpRun) {
        if (g_state != SystemState::ERROR_PUMP) {
            g_stateBeforePumpError = (g_state == SystemState::MANUAL_MODE)
                                     ? SystemState::MANUAL_MODE
                                     : SystemState::AUTO_MODE;
            stopRelay();
            setState(SystemState::ERROR_PUMP);
        }
        return;
    }

    if (g_errors.comm) {
        if (g_state != SystemState::ERROR_COMM) {
            stopRelay();
            setState(SystemState::ERROR_COMM);
        }
        return;
    }

    if (g_errors.timeSync) {
        if (g_state != SystemState::ERROR_TIME) {
            stopRelay();
            setState(SystemState::ERROR_TIME);
        }
        return;
    }
}

void handleLED() {
    // Bypass indication at startup
    if (g_bypass && (millis() - g_bootMs < BYPASS_START_LED_MS)) {
        setLeds(true, true, true);
        return;
    }

    bool g = false;
    bool r = false;
    bool b = false;

    switch (g_state) {
        case SystemState::ERROR_COMM:
            // Communication error: blue 3:3 + red 3:3
            b = blinkRatio(3, 3);
            r = blinkRatio(3, 3);
            setLeds(g, r, b);
            return;

        case SystemState::ERROR_TIME:
            // Time sync error: green 3:3 + red 3:3
            g = blinkRatio(3, 3);
            r = blinkRatio(3, 3);
            setLeds(g, r, b);
            return;

        case SystemState::ERROR_PUMP:
            // Pump run error: red solid + blue 5:5
            r = true;
            b = blinkRatio(5, 5);
            setLeds(g, r, b);
            return;

        default:
            break;
    }

    if (g_state == SystemState::WIFI_CONNECTING || g_state == SystemState::NTP_SYNCING || g_state == SystemState::BLE_PROVISIONING) {
        // During connection/sync: green + blue 5:5
        g = blinkRatio(5, 5);
        b = blinkRatio(5, 5);
        setLeds(g, r, b);
        return;
    }

    if (g_relayOn) {
        // During active relay: green solid, blue 3:7 before feedback OK, then blue solid
        g = true;
        if (g_bypass) b = true;
        else b = g_feedbackOk ? true : blinkRatio(3, 7);
        setLeds(g, r, b);
        return;
    }

    if (g_state == SystemState::AUTO_MODE) {
        g = blinkRatio(1, 9); // automatic mode
    } else if (g_state == SystemState::MANUAL_MODE) {
        g = true; // manual mode
    }

    setLeds(g, r, b);
}

// =========================
// Arduino setup / loop
// =========================
void setup() {
    Serial.begin(115200);
    delay(500);
    LOGI("Booting Grundfos controller");

    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_BLUE, OUTPUT);
    pinMode(PIN_RELAY, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    pinMode(PIN_FEEDBACK, INPUT_PULLUP);

    setLeds(false, false, false);
    digitalWrite(PIN_RELAY, LOW);

    g_bootMs = millis();

    g_prefs.begin(NVS_NS, false);
    loadRuntimeSettingsFromNvs();
    loadWifiFromNvs();
    setupWifiEventLogging();

    setupBleProvisioning();
    setupRestApi();

    // Bypass is active when button is held during startup.
    g_bypass = (digitalRead(PIN_BUTTON) == LOW);
    LOGI("Bypass: %s", g_bypass ? "ACTIVE" : "OFF");

    if (g_wifiSsid.length() == 0) {
        g_bleProvisioningActive = true;
        LOGW("No WiFi credentials in NVS -> BLE provisioning mode");
        setState(SystemState::BLE_PROVISIONING);
    } else {
        g_bleProvisioningActive = false;
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(DEVICE_HOSTNAME);
        logVisibleNetworks();
        WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
        g_wifiStartMs = millis();
        LOGI("WiFi connecting to provisioned SSID: %s", g_wifiSsid.c_str());
        setState(SystemState::WIFI_CONNECTING);
    }

    attachInterrupt(digitalPinToInterrupt(PIN_FEEDBACK), onFeedbackPulse, RISING);
    LOGI("Interrupt on feedback pin initialized");
}

void loop() {
    handleProvisioning();
    handleButton();
    handleWifi();
    handleNTP();
    schedulePump();
    handleFeedback();
    handleRelay();
    handleErrors();
    handleLED();
}