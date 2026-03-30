#pragma once

#include <ESPAsyncWebServer.h>
#include "StateMachine.h"
#include "EventLog.h"
#include "ConfigStorage.h"
#include "WifiManager.h"
#include "AppTypes.h"

class RestApiServer {
public:
    RestApiServer(QueueHandle_t stateQ, StateMachine& sm, EventLog& log, ConfigStorage& cfg, WifiManager& wifi);
    void begin();
    void taskLoop();

private:
    QueueHandle_t _stateQ;
    StateMachine& _sm;
    EventLog& _log;
    ConfigStorage& _cfg;
    WifiManager& _wifi;
    AsyncWebServer _server;

    void sendOk(AsyncWebServerRequest* req);
};
