#include "TimeManager.h"
#include <time.h>
#include <WiFi.h>

TimeManager::TimeManager(QueueHandle_t stateQ)
    : _stateQ(stateQ), _synced(false), _lastSyncMs(0) {}

void TimeManager::begin() {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();    
}

bool TimeManager::isSynced() const {
    time_t now = time(nullptr);
    return now > 1700000000;
}

void TimeManager::taskLoop() {
    uint32_t nowMs = millis();
    if (WiFi.status() == WL_CONNECTED && (nowMs - _lastSyncMs > 21600000 || !_synced)) {
        _lastSyncMs = nowMs;
        _synced = isSynced();

        StateEvent ev{};
        if (_synced) ev.type = StateEventType::TIME_SYNC_OK;
        else ev.type = StateEventType::TIME_SYNC_FAIL;
        Serial.printf("Time sync %s\n", _synced ? "successful" : "failed");
        time_t now = time(nullptr);
        struct tm* timeinfo = localtime(&now);
        Serial.printf("%04d-%02d-%02d %02d:%02d:%02d\n",
        timeinfo->tm_year + 1900,
        timeinfo->tm_mon + 1,
        timeinfo->tm_mday,
        timeinfo->tm_hour,
        timeinfo->tm_min,
        timeinfo->tm_sec);
        xQueueSend(_stateQ, &ev, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}