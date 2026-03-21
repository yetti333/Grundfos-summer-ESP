#pragma once
#include <Arduino.h>
#include "AppTypes.h"

class RelayController {
public:
    void begin() {
        pinMode(PIN_RELAY, OUTPUT);
        digitalWrite(PIN_RELAY, LOW);
    }

    void set(bool on) {
        digitalWrite(PIN_RELAY, on ? HIGH : LOW);
    }
};