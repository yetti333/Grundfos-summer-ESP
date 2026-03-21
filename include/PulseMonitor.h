#pragma once

#include "AppTypes.h"

class PulseMonitor {
public:
    PulseMonitor(QueueHandle_t pulseQ, QueueHandle_t stateQ);
    void begin();
    void taskLoop();

private:
    static void IRAM_ATTR isrPulse();
    static volatile uint32_t s_pulseCounter;
    static volatile uint32_t s_lastPulseMs;

    QueueHandle_t _pulseQ;
    QueueHandle_t _stateQ;
    uint32_t _lastCounter;
};