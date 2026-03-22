#include "ButtonHandler.h"

ButtonHandler::ButtonHandler(QueueHandle_t buttonQ, QueueHandle_t stateQ)
    : _buttonQ(buttonQ), _stateQ(stateQ), _stablePressed(false), _lastRawPressed(false),
      _lastDebounceMs(0), _pressStartMs(0), _longSent(false), _vLongSent(false) {}

void ButtonHandler::begin() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
}

void ButtonHandler::taskLoop() {
    const uint32_t now = millis();
    bool rawPressed = (digitalRead(PIN_BUTTON) == LOW);

    if (rawPressed != _lastRawPressed) {
        _lastDebounceMs = now;
        _lastRawPressed = rawPressed;
    }

    if (now - _lastDebounceMs > 50) {
        if (rawPressed != _stablePressed) {
            _stablePressed = rawPressed;
            if (_stablePressed) {
                _pressStartMs = now;
                _longSent = false;
                _vLongSent = false;
            } else {
                uint32_t dur = now - _pressStartMs;
                ButtonEvent ev{ButtonEventType::RELEASE, dur};
                xQueueSend(_buttonQ, &ev, 0);
                StateEvent sev{StateEventType::BUTTON_RELEASE, (int32_t)dur, 0, 0, false};
                xQueueSend(_stateQ, &sev, 0);

                if (dur < 1000) {
                    ButtonEvent sh{ButtonEventType::SHORT_PRESS, dur};
                    Serial.println("Short press detected");
                    xQueueSend(_buttonQ, &sh, 0);
                    StateEvent s2{StateEventType::BUTTON_SHORT, (int32_t)dur, 0, 0, false};
                    xQueueSend(_stateQ, &s2, 0);
                }
            }
        }
    }

    if (_stablePressed) {
        uint32_t dur = now - _pressStartMs;
        if (!_longSent && dur >= 3000) {
            _longSent = true;
            ButtonEvent ev{ButtonEventType::LONG_PRESS, dur};
            Serial.println("Long press detected");
            xQueueSend(_buttonQ, &ev, 0);
            StateEvent sev{StateEventType::BUTTON_LONG, (int32_t)dur, 0, 0, false};
            xQueueSend(_stateQ, &sev, 0);
        }
        if (!_vLongSent && dur >= 5000) {
            _vLongSent = true;
            ButtonEvent ev{ButtonEventType::VERY_LONG_PRESS, dur};
            Serial.println("Very long press detected");
            xQueueSend(_buttonQ, &ev, 0);
            StateEvent sev{StateEventType::BUTTON_VLONG, (int32_t)dur, 0, 0, false};
            xQueueSend(_stateQ, &sev, 0);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
}