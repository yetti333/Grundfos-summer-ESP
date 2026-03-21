#pragma once
#include "AppTypes.h"
#include "RelayController.h"

class PumpControl {
public:
    PumpControl(QueueHandle_t cmdQ, QueueHandle_t stateQ, EventGroupHandle_t eg);
    void begin();
    void taskLoop();
    bool isRunning() const;

private:
    QueueHandle_t _cmdQ;
    QueueHandle_t _stateQ;
    EventGroupHandle_t _eg;
    RelayController _relay;

    bool _running;
    uint32_t _startMs;
    uint32_t _durationSec;
    uint32_t _testSec;
    bool _testDoneSent;
};