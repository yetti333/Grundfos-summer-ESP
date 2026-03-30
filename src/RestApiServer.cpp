#include "RestApiServer.h"
#include <ArduinoJson.h>
#include <esp_system.h>

namespace {
const char* wifiStatusToString(wl_status_t status) {
    switch (status) {
        case WL_NO_SHIELD: return "NO_SHIELD";
        case WL_IDLE_STATUS: return "IDLE";
        case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
        case WL_CONNECTED: return "CONNECTED";
        case WL_CONNECT_FAILED: return "CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED: return "DISCONNECTED";
        default: return "UNKNOWN";
    }
}
}

RestApiServer::RestApiServer(QueueHandle_t stateQ, StateMachine& sm, EventLog& log, ConfigStorage& cfg, WifiManager& wifi)
    : _stateQ(stateQ), _sm(sm), _log(log), _cfg(cfg), _wifi(wifi), _server(80) {}

void RestApiServer::sendJson(AsyncWebServerRequest* req, int code, const String& body) {
    AsyncWebServerResponse* resp = req->beginResponse(code, "application/json", body);
    resp->addHeader("Content-Type", "application/json");
    resp->addHeader("Connection", "keep-alive");
    req->send(resp);
}

void RestApiServer::sendOk(AsyncWebServerRequest* req) {
    sendJson(req, 200, "{\"ok\":true}");
}

void RestApiServer::begin() {
    DefaultHeaders::Instance().addHeader("Connection", "keep-alive");

    _server.on("/heartbeat", HTTP_GET, [this](AsyncWebServerRequest* req) {
        String hb, st;
        _sm.getSnapshotJson(hb, st);
        sendJson(req, 200, hb);
    });

    _server.on("/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        String hb, st;
        _sm.getSnapshotJson(hb, st);
        sendJson(req, 200, st);
    });

    _server.on("/diag/ping", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["ok"] = true;
        doc["uptime_sec"] = millis() / 1000U;
        doc["free_heap"] = ESP.getFreeHeap();

        String out;
        serializeJson(doc, out);
        sendJson(req, 200, out);
    });

    _server.on("/diag/network", HTTP_GET, [this](AsyncWebServerRequest* req) {
        wl_status_t status = WiFi.status();
        JsonDocument doc;

        doc["ok"] = true;
        doc["wifi_connected"] = _wifi.connected();
        doc["wifi_status_code"] = static_cast<int>(status);
        doc["wifi_status_text"] = wifiStatusToString(status);
        doc["provisioning_active"] = _wifi.provisioningActive();
        doc["ssid"] = WiFi.SSID();
        doc["rssi_dbm"] = _wifi.rssi();
        doc["ip"] = WiFi.localIP().toString();
        doc["gateway"] = WiFi.gatewayIP().toString();
        doc["subnet"] = WiFi.subnetMask().toString();
        doc["dns"] = WiFi.dnsIP().toString();
        doc["mac"] = WiFi.macAddress();
        doc["hostname"] = _wifi.hostname();
        doc["mdns_running"] = _wifi.mdnsRunning();
        doc["mdns_host"] = _wifi.mdnsHostname();
        doc["mdns_url"] = _wifi.mdnsUrl();
        doc["http_port"] = 80;
        doc["http_ip_url"] = String("http://") + WiFi.localIP().toString() + "/";
        doc["uptime_sec"] = millis() / 1000U;

        String out;
        serializeJson(doc, out);
        sendJson(req, 200, out);
    });

    _server.on("/log", HTTP_GET, [this](AsyncWebServerRequest* req) {
        LogEvent temp[64];
        size_t n = _log.copy(temp, 64);

        JsonDocument doc;
        JsonArray arr = doc["events"].to<JsonArray>();
        for (size_t i = 0; i < n; i++) {
            JsonObject e = arr.add<JsonObject>();
            e["timestamp"] = temp[i].timestamp;
            e["type"] = temp[i].type;
            e["detail"] = temp[i].detail;
        }

        String out;
        serializeJson(doc, out);
        sendJson(req, 200, out);
    });

    auto postJson = [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, std::function<void(JsonVariantConst)> fn) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            sendJson(req, 400, "{\"error\":\"invalid_json\"}");
            return;
        }
        fn(doc.as<JsonVariantConst>());
    };

    _server.on("/set/schedule", HTTP_POST, [this](AsyncWebServerRequest* req) {}, nullptr,
        [this, postJson](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            postJson(req, data, len, [this, req](JsonVariantConst v) {
                StateEvent e{};
                e.type = StateEventType::API_SET_SCHEDULE;
                e.a = v["start_hour"] | 19;
                e.b = v["start_minute"] | 0;
                e.c = v["duration_minutes"] | 5;
                xQueueSend(_stateQ, &e, 0);
                sendOk(req);
            });
        });

    _server.on("/set/mode", HTTP_POST, [this](AsyncWebServerRequest* req) {}, nullptr,
        [this, postJson](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            postJson(req, data, len, [this, req](JsonVariantConst v) {
                const char* mode = v["mode"] | "AUTO";
                StateEvent e{};
                e.type = (String(mode) == "AUTO") ? StateEventType::API_SET_MODE_AUTO : StateEventType::API_SET_MODE_MANUAL;
                xQueueSend(_stateQ, &e, 0);
                sendOk(req);
            });
        });

    _server.on("/set/bypass", HTTP_POST, [this](AsyncWebServerRequest* req) {}, nullptr,
        [this, postJson](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            postJson(req, data, len, [this, req](JsonVariantConst v) {
                bool on = v["bypass"] | false;
                StateEvent e{};
                e.type = on ? StateEventType::API_SET_BYPASS_ON : StateEventType::API_SET_BYPASS_OFF;
                xQueueSend(_stateQ, &e, 0);
                sendOk(req);
            });
        });

    _server.on("/bypass", HTTP_GET, [this](AsyncWebServerRequest* req) {
        sendJson(req, 200, String("{\"bypass\":") + (_sm.isBypassActive() ? "true" : "false") + "}");
    });

    _server.on("/pump/start", HTTP_POST, [this](AsyncWebServerRequest* req) {
        StateEvent e{StateEventType::API_PUMP_START, 0, 0, 0, false};
        xQueueSend(_stateQ, &e, 0);
        sendOk(req);
    });

    _server.on("/pump/stop", HTTP_POST, [this](AsyncWebServerRequest* req) {
        StateEvent e{StateEventType::API_PUMP_STOP, 0, 0, 0, false};
        xQueueSend(_stateQ, &e, 0);
        sendOk(req);
    });

    _server.on("/pump/error-reset", HTTP_POST, [this](AsyncWebServerRequest* req) {
        StateEvent e{StateEventType::API_RESET_PUMP_ERROR, 0, 0, 0, false};
        xQueueSend(_stateQ, &e, 0);
        sendOk(req);
    });

    _server.on("/provision", HTTP_POST, [this](AsyncWebServerRequest* req) {}, nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                sendJson(req, 400, "{\"error\":\"invalid_json\"}");
                return;
            }

            String ssid = doc["ssid"] | "";
            String pass = doc["password"] | "";
            ssid.trim();

            if (ssid.isEmpty()) {
                sendJson(req, 400, "{\"error\":\"ssid_empty\"}");
                return;
            }

            Serial.printf("[Provisioning] ssid=%s\n", ssid.c_str());
            _cfg.saveWifi(ssid, pass);
            StateEvent e{StateEventType::WIFI_PROVISION_DONE, 0, 0, 0, false};
            xQueueSend(_stateQ, &e, 0);

            sendJson(req, 200, "{\"ok\":true}");

            xTaskCreate([](void*) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }, "restart_task", 2048, nullptr, 1, nullptr);
        });

    _server.begin();
}

void RestApiServer::taskLoop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
