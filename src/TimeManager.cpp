#include "TimeManager.h"
#include <time.h>
#include <WiFi.h>

TimeManager::TimeManager(QueueHandle_t stateQ)
    : _stateQ(stateQ), _synced(false), _lastSyncMs(0) {}

void TimeManager::begin() {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
}

bool TimeManager::isSynced() const {
    time_t now = time(nullptr);
    return now > 1700000000;
}

void TimeManager::taskLoop() {
    uint32_t nowMs = millis();
    if (WiFi.status() == WL_CONNECTED && (nowMs - _lastSyncMs > 60000 || !_synced)) {
        _lastSyncMs = nowMs;
        _synced = isSynced();

        StateEvent ev{};
        if (_synced) ev.type = StateEventType::TIME_SYNC_OK;
        else ev.type = StateEventType::TIME_SYNC_FAIL;
        xQueueSend(_stateQ, &ev, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}