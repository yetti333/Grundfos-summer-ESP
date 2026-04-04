#include "PulseMonitor.h"
#include <time.h>

namespace {
constexpr uint32_t kSamplePeriodMs = 200;
constexpr uint32_t kPulseMissingTimeoutUs = 200000;
constexpr uint32_t kTargetFreqX100 = 7500;
constexpr uint32_t kMinFreqX100 = 7125;
constexpr uint32_t kMaxFreqX100 = 7875;
constexpr uint32_t kMinPeriodUs = 100000000UL / kMaxFreqX100;
constexpr uint32_t kMaxPeriodUs = 100000000UL / kMinFreqX100;
}

volatile uint32_t PulseMonitor::s_riseCount = 0;
volatile uint32_t PulseMonitor::s_lastRiseUs = 0;
volatile uint32_t PulseMonitor::s_lastPeriodUs = 0;
volatile uint32_t PulseMonitor::s_lastHighUs = 0;
volatile bool PulseMonitor::s_lastLevelHigh = false;

PulseMonitor::PulseMonitor(QueueHandle_t pulseQ, QueueHandle_t stateQ)
    : _pulseQ(pulseQ), _stateQ(stateQ), _lastRiseCount(0) {}

void IRAM_ATTR PulseMonitor::isrPulse() {
    const uint32_t nowUs = micros();
    const bool levelHigh = (digitalRead(PIN_PULSE) == HIGH);

    if (levelHigh && !s_lastLevelHigh) {
        if (s_lastRiseUs > 0) {
            s_lastPeriodUs = nowUs - s_lastRiseUs;
        }
        s_lastRiseUs = nowUs;
        s_riseCount++;
    } else if (!levelHigh && s_lastLevelHigh) {
        if (s_lastRiseUs > 0) {
            s_lastHighUs = nowUs - s_lastRiseUs;
        }
    }

    s_lastLevelHigh = levelHigh;
}

void PulseMonitor::begin() {
    pinMode(PIN_PULSE, INPUT_PULLUP);
    s_lastLevelHigh = (digitalRead(PIN_PULSE) == HIGH);
    attachInterrupt(digitalPinToInterrupt(PIN_PULSE), isrPulse, CHANGE);
}

PulseInterpretedState PulseMonitor::interpretDuty(uint16_t dutyX100, bool validFrequency, bool pulsePresent) {
    if (!pulsePresent) return PulseInterpretedState::PULSE_MISSING;
    if (!validFrequency) return PulseInterpretedState::INVALID_FREQUENCY;

    if (dutyX100 <= 200) return PulseInterpretedState::STANDBY;
    if (dutyX100 < 500) return PulseInterpretedState::LOW_OPERATION;

    if (dutyX100 >= 9900) return PulseInterpretedState::ALARM_ELECTRICAL_FAULT;
    if (dutyX100 >= 8800 && dutyX100 <= 9200) return PulseInterpretedState::ALARM_ROTOR_BLOCKED;
    if (dutyX100 >= 7300 && dutyX100 <= 7700) return PulseInterpretedState::ALARM_LOW_VOLTAGE;

    return PulseInterpretedState::NORMAL_OPERATION;
}

void PulseMonitor::taskLoop() {
    const bool relayOn = (digitalRead(PIN_RELAY) == HIGH);

    uint32_t riseCount = 0;
    uint32_t lastRiseUs = 0;
    uint32_t lastPeriodUs = 0;
    uint32_t lastHighUs = 0;
    noInterrupts();
    riseCount = s_riseCount;
    lastRiseUs = s_lastRiseUs;
    lastPeriodUs = s_lastPeriodUs;
    lastHighUs = s_lastHighUs;
    interrupts();

    uint32_t deltaRise = riseCount - _lastRiseCount;
    _lastRiseCount = riseCount;

    if (!relayOn) {
        // Ignore stale edge history while the relay is off.
        lastRiseUs = 0;
        lastPeriodUs = 0;
        lastHighUs = 0;
    }

    const uint32_t nowUs = micros();
    const bool pulsePresent = relayOn && lastRiseUs > 0 && (nowUs - lastRiseUs) <= kPulseMissingTimeoutUs;
    const bool validFrequency = pulsePresent && lastPeriodUs >= kMinPeriodUs && lastPeriodUs <= kMaxPeriodUs;

    uint16_t frequencyHz = 0;
    if (lastPeriodUs > 0) {
        frequencyHz = (uint16_t)((1000000UL + (lastPeriodUs / 2U)) / lastPeriodUs);
    }

    uint16_t dutyX100 = 0;
    if (lastPeriodUs > 0) {
        uint32_t clampedHighUs = (lastHighUs > lastPeriodUs) ? lastPeriodUs : lastHighUs;
        dutyX100 = (uint16_t)((clampedHighUs * 10000UL + (lastPeriodUs / 2U)) / lastPeriodUs);
        if (dutyX100 > 10000) dutyX100 = 10000;
    }

    uint32_t measuredFreqX100 = 0;
    if (lastPeriodUs > 0) {
        measuredFreqX100 = 100000000UL / lastPeriodUs;
    }
    int freqErr = abs((int)measuredFreqX100 - (int)kTargetFreqX100);
    int stability = 100 - (freqErr * 100 / (int)kTargetFreqX100);
    if (stability < 0) stability = 0;
    if (stability > 100) stability = 100;

    PulseInterpretedState interpreted = interpretDuty(dutyX100, validFrequency, pulsePresent);

    PulseEvent p{};
    p.countLastSecond = (deltaRise * 1000U) / kSamplePeriodMs;
    p.frequencyHz = frequencyHz;
    p.stabilityPercent = (uint8_t)stability;
    p.pulseOk = pulsePresent;
    p.lastPulseUnix = pulsePresent ? (uint32_t)time(nullptr) : 0;
    p.periodUs = lastPeriodUs;
    p.highUs = lastHighUs;
    p.dutyX100 = dutyX100;
    p.validFrequency = validFrequency;
    p.interpretedState = (uint8_t)interpreted;

    xQueueSend(_pulseQ, &p, 0);

    StateEvent sev{};
    sev.type = StateEventType::PULSE_UPDATE;
    sev.a = (int32_t)frequencyHz;
    sev.b = (int32_t)dutyX100;
    sev.c = (int32_t)lastPeriodUs;
    sev.flag = pulsePresent;
    sev.d = (int32_t)lastHighUs;
    sev.e = (int32_t)interpreted;
    sev.ts = (uint32_t)time(nullptr);
    sev.flag2 = validFrequency;
    xQueueSend(_stateQ, &sev, 0);

    if (relayOn) {
        Serial.printf("[PWM] period_ms=%.3f high_ms=%.3f duty=%.2f%% freq=%uHz valid_freq=%d state=%s ts=%lu\n",
                      lastPeriodUs / 1000.0f,
                      lastHighUs / 1000.0f,
                      dutyX100 / 100.0f,
                      frequencyHz,
                      validFrequency ? 1 : 0,
                      pulseStateToString(interpreted),
                      (unsigned long)sev.ts);
    }

    vTaskDelay(pdMS_TO_TICKS(kSamplePeriodMs));
}