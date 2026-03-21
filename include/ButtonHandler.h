#pragma once

#include "AppTypes.h"

class ButtonHandler {
public:
    ButtonHandler(QueueHandle_t buttonQ, QueueHandle_t stateQ);
    void begin();
    void taskLoop();

private:
    QueueHandle_t _buttonQ;
    QueueHandle_t _stateQ;
    bool _stablePressed;
    bool _lastRawPressed;
    uint32_t _lastDebounceMs;
    uint32_t _pressStartMs;
    bool _longSent;
    bool _vLongSent;
};