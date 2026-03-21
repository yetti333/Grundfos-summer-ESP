#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct LogEvent {
    uint32_t timestamp;
    String type;
    String detail;
};

class EventLog {
public:
    static constexpr size_t CAPACITY = 100;

    EventLog();
    void add(const String& type, const String& detail);
    size_t copy(LogEvent* out, size_t maxItems);

private:
    LogEvent _events[CAPACITY];
    size_t _head;
    size_t _count;
    SemaphoreHandle_t _mtx;
};