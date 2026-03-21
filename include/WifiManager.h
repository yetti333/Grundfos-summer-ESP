#pragma once

#include <WiFi.h>
#include <ESPmDNS.h>
#include "AppTypes.h"
#include "ConfigStorage.h"

class WifiManager {
public:
    WifiManager(QueueHandle_t stateQ, ConfigStorage& storage);
    void begin(bool forceProvision);
    void taskLoop();

    int32_t rssi() const;
    bool connected() const;

private:
    bool loadCredentials();
    void connectIfNeeded();
    void startProvisionAp();

    QueueHandle_t _stateQ;
    ConfigStorage& _storage;
    String _ssid;
    String _pass;
    bool _provisioning;
    uint32_t _lastAttemptMs;
};