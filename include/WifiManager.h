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
    void exitProvisioning();

    int32_t rssi() const;
    bool connected() const;
    bool provisioningActive() const;
    bool mdnsRunning() const;
    const char* hostname() const;
    const char* mdnsHostname() const;
    String mdnsUrl() const;

private:
    bool loadCredentials();
    void connectIfNeeded();
    void startProvisionAp();
    void startMdns();
    void stopMdns();
    void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

    QueueHandle_t _stateQ;
    ConfigStorage& _storage;
    String _ssid;
    String _pass;
    bool _provisioning;
    bool _mdnsRunning;
    uint32_t _lastAttemptMs;
};
