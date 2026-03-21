#pragma once

#include "AppTypes.h"

class LedController {
public:
    LedController(QueueHandle_t ledQueue);
    void begin();
    void taskLoop();

private:
    struct BlinkCfg {
        bool g;
        bool r;
        bool b;
        uint16_t onMs;
        uint16_t offMs;
        bool solid;
    };

    void setRaw(bool g, bool r, bool b);
    BlinkCfg cfgFor(LedPattern p);

    QueueHandle_t _queue;
    LedPattern _current;
    uint32_t _lastToggle;
    bool _phaseOn;
};