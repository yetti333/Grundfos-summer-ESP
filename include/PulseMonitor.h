#pragma once

#include "AppTypes.h"

class PulseMonitor {
public:
    PulseMonitor(QueueHandle_t pulseQ, QueueHandle_t stateQ);
    void begin();
    void taskLoop();

private:
    static void IRAM_ATTR isrPulse();
    static PulseInterpretedState interpretDuty(uint16_t dutyX100, bool validFrequency, bool pulsePresent);

    static volatile uint32_t s_riseCount;
    static volatile uint32_t s_lastRiseUs;
    static volatile uint32_t s_lastPeriodUs;
    static volatile uint32_t s_lastHighUs;
    static volatile bool s_lastLevelHigh;

    QueueHandle_t _pulseQ;
    QueueHandle_t _stateQ;
    uint32_t _lastRiseCount;
};