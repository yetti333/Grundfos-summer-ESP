#include "EventLog.h"
#include <time.h>

EventLog::EventLog() : _head(0), _count(0) {
    _mtx = xSemaphoreCreateMutex();
}

void EventLog::add(const String& type, const String& detail) {
    if (!_mtx) return;
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t idx = (_head + _count) % CAPACITY;
        _events[idx].timestamp = static_cast<uint32_t>(time(nullptr));
        _events[idx].type = type;
        _events[idx].detail = detail;
        if (_count < CAPACITY) {
            _count++;
        } else {
            _head = (_head + 1) % CAPACITY;
        }
        xSemaphoreGive(_mtx);
    }
}

size_t EventLog::copy(LogEvent* out, size_t maxItems) {
    if (!_mtx || !out || maxItems == 0) return 0;
    size_t copied = 0;
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t n = (_count < maxItems) ? _count : maxItems;
        for (size_t i = 0; i < n; i++) {
            size_t idx = (_head + i) % CAPACITY;
            out[i] = _events[idx];
        }
        copied = n;
        xSemaphoreGive(_mtx);
    }
    return copied;
}