#include "RestApiServer.h"
#include <ArduinoJson.h>

RestApiServer::RestApiServer(QueueHandle_t stateQ, StateMachine& sm, EventLog& log, ConfigStorage& cfg)
    : _stateQ(stateQ), _sm(sm), _log(log), _cfg(cfg), _server(80) {}

void RestApiServer::sendOk(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"ok\":true}");
}

void RestApiServer::begin() {
    _server.on("/heartbeat", HTTP_GET, [this](AsyncWebServerRequest* req) {
        String hb, st;
        _sm.getSnapshotJson(hb, st);
        req->send(200, "application/json", hb);
    });

    _server.on("/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        String hb, st;
        _sm.getSnapshotJson(hb, st);
        req->send(200, "application/json", st);
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
        req->send(200, "application/json", out);
    });

    auto postJson = [](AsyncWebServerRequest* req, std::function<void(JsonVariantConst)> fn) {
        if (!req->hasParam("plain", true)) {
            req->send(400, "application/json", "{\"ok\":false}");
            return;
        }
        String body = req->getParam("plain", true)->value();
        JsonDocument doc;
        if (deserializeJson(doc, body) != DeserializationError::Ok) {
            req->send(400, "application/json", "{\"ok\":false}");
            return;
        }
        fn(doc.as<JsonVariantConst>());
    };

    _server.on("/set/schedule", HTTP_POST, [this](AsyncWebServerRequest* req) {}, nullptr,
        [this, postJson](AsyncWebServerRequest* req, uint8_t*, size_t, size_t, size_t) {
            postJson(req, [this, req](JsonVariantConst v) {
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
        [this, postJson](AsyncWebServerRequest* req, uint8_t*, size_t, size_t, size_t) {
            postJson(req, [this, req](JsonVariantConst v) {
                const char* mode = v["mode"] | "AUTO";
                StateEvent e{};
                e.type = (String(mode) == "AUTO") ? StateEventType::API_SET_MODE_AUTO : StateEventType::API_SET_MODE_MANUAL;
                xQueueSend(_stateQ, &e, 0);
                sendOk(req);
            });
        });

    _server.on("/set/bypass", HTTP_POST, [this](AsyncWebServerRequest* req) {}, nullptr,
        [this, postJson](AsyncWebServerRequest* req, uint8_t*, size_t, size_t, size_t) {
            postJson(req, [this, req](JsonVariantConst v) {
                bool on = v["bypass"] | false;
                StateEvent e{};
                e.type = on ? StateEventType::API_SET_BYPASS_ON : StateEventType::API_SET_BYPASS_OFF;
                xQueueSend(_stateQ, &e, 0);
                sendOk(req);
            });
        });

    _server.on("/bypass", HTTP_GET, [this](AsyncWebServerRequest* req) {
        req->send(200, "application/json", String("{\"bypass\":") + (_sm.isBypassActive() ? "true" : "false") + "}");
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

    _server.on("/provision", HTTP_POST, [this](AsyncWebServerRequest* req) {}, nullptr,
        [this, postJson](AsyncWebServerRequest* req, uint8_t*, size_t, size_t, size_t) {
            postJson(req, [this, req](JsonVariantConst v) {
                String ssid = v["ssid"] | "";
                String pass = v["password"] | "";
                if (ssid.isEmpty()) {
                    req->send(400, "application/json", "{\"ok\":false}");
                    return;
                }
                _cfg.saveWifi(ssid, pass);
                sendOk(req);
            });
        });

    _server.begin();
}

void RestApiServer::taskLoop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}