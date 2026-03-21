#pragma once
#include "AppTypes.h"

class TimeManager {
public:
    explicit TimeManager(QueueHandle_t stateQ);
    void begin();
    void taskLoop();
    bool isSynced() const;

private:
    QueueHandle_t _stateQ;
    bool _synced;
    uint32_t _lastSyncMs;
};