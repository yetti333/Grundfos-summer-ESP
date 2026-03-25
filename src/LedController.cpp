#include "LedController.h"

LedController::LedController(QueueHandle_t ledQueue)
    : _queue(ledQueue), _current(LedPattern::OFF), _lastToggle(0), _phaseOn(false) {}

void LedController::begin() {
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_LED_BLUE, OUTPUT);
    setRaw(false, false, false);
}

void LedController::setRaw(bool g, bool r, bool b) {
    digitalWrite(PIN_LED_GREEN, g ? HIGH : LOW);
    digitalWrite(PIN_LED_RED, r ? HIGH : LOW);
    digitalWrite(PIN_LED_BLUE, b ? HIGH : LOW);
}
// LED patterns: green, red, blue, on duration ms, off duration ms, solid
LedController::BlinkCfg LedController::cfgFor(LedPattern p) {
    switch (p) {
        case LedPattern::ALL_SOLID:   return {true, true, true, 0, 0, true};
        case LedPattern::WIFI_CONNECT:return {true, false, true, 500, 500, false};
        case LedPattern::WIFI_ERROR:  return {false, true, true, 30, 30, false};
        case LedPattern::TIME_ERROR:  return {false, true, true, 30, 30, false};
        case LedPattern::AUTO_IDLE:   return {true, false, false, 40, 1000, false};
        case LedPattern::MANUAL_IDLE: return {true, false, false, 0, 0, true};
        case LedPattern::BYPASS_IDLE: return {true, false, true, 500, 500, false};
        case LedPattern::PUMP_RUNNING:return {true, false, true, 0, 0, true};
        case LedPattern::PUMP_ERROR:  return {false, true, false, 30, 30, false};
        case LedPattern::OFF:
        default:                      return {false, false, false, 0, 0, true};
    }
}

void LedController::taskLoop() {
    LedPattern p;
    if (xQueueReceive(_queue, &p, 0) == pdTRUE) {
        _current = p;
        _lastToggle = millis();
        _phaseOn = true;
    }

    BlinkCfg c = cfgFor(_current);
    if (c.solid) {
        setRaw(c.g, c.r, c.b);
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }

    uint32_t now = millis();
    uint32_t interval = _phaseOn ? c.onMs : c.offMs;
    if (now - _lastToggle >= interval) {
        _phaseOn = !_phaseOn;
        _lastToggle = now;
    }

    if (_phaseOn) setRaw(c.g, c.r, c.b);
    else setRaw(false, false, false);

    vTaskDelay(pdMS_TO_TICKS(20));
}