#include "PulseMonitor.h"
#include <time.h>

volatile uint32_t PulseMonitor::s_pulseCounter = 0;
volatile uint32_t PulseMonitor::s_lastPulseMs = 0;

PulseMonitor::PulseMonitor(QueueHandle_t pulseQ, QueueHandle_t stateQ)
    : _pulseQ(pulseQ), _stateQ(stateQ), _lastCounter(0) {}

void IRAM_ATTR PulseMonitor::isrPulse() {
    s_pulseCounter++;
    s_lastPulseMs = millis();
}

void PulseMonitor::begin() {
    pinMode(PIN_PULSE, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_PULSE), isrPulse, RISING);
}

void PulseMonitor::taskLoop() {
    uint32_t current = s_pulseCounter;
    uint32_t delta = current - _lastCounter;
    _lastCounter = current;

    uint16_t hz = (uint16_t)delta;
    int err = abs((int)hz - 75);
    int st = 100 - (err * 100 / 75);
    if (st < 0) st = 0;
    if (st > 100) st = 100;
    bool pulseOk = hz >= 10;

    PulseEvent p{};
    p.countLastSecond = delta;
    p.frequencyHz = hz;
    p.stabilityPercent = (uint8_t)st;
    p.pulseOk = pulseOk;
    p.lastPulseUnix = pulseOk ? (uint32_t)time(nullptr) : 0;

    xQueueSend(_pulseQ, &p, 0);

    StateEvent sev{};
    sev.type = StateEventType::PULSE_UPDATE;
    sev.a = hz;
    sev.b = p.stabilityPercent;
    sev.c = delta;
    sev.flag = pulseOk;
    xQueueSend(_stateQ, &sev, 0);

    vTaskDelay(pdMS_TO_TICKS(1000));
}