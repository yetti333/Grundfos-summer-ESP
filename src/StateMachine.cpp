#include "StateMachine.h"
#include <ArduinoJson.h>
#include <time.h>

StateMachine::StateMachine(QueueHandle_t stateQ, QueueHandle_t ledQ, QueueHandle_t pumpCmdQ, EventGroupHandle_t eg,
                           ConfigStorage& cfg, EventLog& log)
    : _stateQ(stateQ), _ledQ(ledQ), _pumpCmdQ(pumpCmdQ), _eg(eg), _cfg(cfg), _log(log),
      _state(SystemState::BOOT), _wifiErr(false), _timeErr(false), _pumpErr(false),
      _bypass(false), _bypassApiValue(false), _manual(false), _wifiConnected(false), _wifiRssi(-127), _pulseOk(false),
      _pulseHz(0), _pulseCountLastMin(0), _pulseStability(0), _lastPulseTs(0), _uptimeStart(millis()),
      _lastAutoMinute(-1) {
    _mtx = xSemaphoreCreateMutex();
}

void StateMachine::begin() {
    _cfg.loadSchedule(_schedule);
    updateBypass();
    setState(SystemState::BOOT);
}

SystemState StateMachine::state() const {
    return _state;
}

ScheduleConfig StateMachine::schedule() const {
    return _schedule;
}

void StateMachine::setWifiInfo(bool connected, int32_t rssi) {
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        _wifiConnected = connected;
        _wifiRssi = rssi;
        xSemaphoreGive(_mtx);
    }
}

void StateMachine::setPulseInfo(uint16_t hz, uint32_t count, uint8_t stability, bool ok, uint32_t lastTs) {
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        _pulseHz = hz;
        _pulseCountLastMin = count * 60;
        _pulseStability = stability;
        _pulseOk = ok;
        if (lastTs > 0) _lastPulseTs = lastTs;
        xSemaphoreGive(_mtx);
    }
}

void StateMachine::setLed(LedPattern p) {
    xQueueSend(_ledQ, &p, 0);
}

void StateMachine::setState(SystemState s) {
    _state = s;
    switch (s) {
        case SystemState::WIFI_CONNECT: setLed(LedPattern::WIFI_CONNECT); break;
        case SystemState::WIFI_ERROR: setLed(LedPattern::WIFI_ERROR); break;
        case SystemState::TIME_ERROR: setLed(LedPattern::TIME_ERROR); break;
        case SystemState::AUTO_MODE: setLed(LedPattern::AUTO_IDLE); break;
        case SystemState::MANUAL_MODE: setLed(LedPattern::MANUAL_IDLE); break;
        case SystemState::BYPASS_MODE: setLed(LedPattern::BYPASS_IDLE); break;
        case SystemState::PUMP_RUNNING: setLed(LedPattern::PUMP_RUNNING); break;
        case SystemState::PUMP_ERROR: setLed(LedPattern::PUMP_ERROR); break;
        case SystemState::BOOT: setLed(LedPattern::ALL_SOLID); break;
        default: setLed(LedPattern::OFF); break;
    }
}

void StateMachine::startPumpAuto() {
    PumpCommand c{PumpCommandType::START_AUTO, (uint32_t)_schedule.durationMinutes * 60U, 0};
    xQueueSend(_pumpCmdQ, &c, 0);
    setState(SystemState::PUMP_RUNNING);
    _log.add("PUMP_START", "Automatic schedule");
}

void StateMachine::startPumpManual() {
    PumpCommand c{PumpCommandType::START_MANUAL, 0, 0};
    xQueueSend(_pumpCmdQ, &c, 0);
    setState(SystemState::PUMP_RUNNING);
    _log.add("PUMP_START", "Manual");
}

void StateMachine::stopPump() {
    PumpCommand c{PumpCommandType::STOP, 0, 0};
    xQueueSend(_pumpCmdQ, &c, 0);
    if (_manual) setState(SystemState::MANUAL_MODE);
    else setState(SystemState::AUTO_MODE);
    _log.add("PUMP_STOP", "Requested");
}

void StateMachine::updateBypass() {
    _bypass = _manual || _bypassApiValue;
}

void StateMachine::checkAutoSchedule() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);

    int currentMinute = t.tm_hour * 60 + t.tm_min;
    int scheduleMinute = _schedule.startHour * 60 + _schedule.startMinute;

    if (currentMinute == scheduleMinute && _lastAutoMinute != currentMinute) {
        _lastAutoMinute = currentMinute;
        startPumpAuto();
    }
}

void StateMachine::taskLoop() {
    StateEvent ev{};
    if (xQueueReceive(_stateQ, &ev, pdMS_TO_TICKS(200)) == pdTRUE) {
        switch (ev.type) {
            case StateEventType::WIFI_CONNECTED:
                _wifiErr = false;
                setWifiInfo(true, ev.a);
                xEventGroupSetBits(_eg, WIFI_OK_BIT);
                if (_state == SystemState::WIFI_CONNECT || _state == SystemState::WIFI_ERROR || _state == SystemState::BOOT) {
                    setState(SystemState::TIME_SYNC);
                }
                break;

            case StateEventType::WIFI_DISCONNECTED:
                setWifiInfo(false, -127);
                _wifiErr = true;
                xEventGroupClearBits(_eg, WIFI_OK_BIT);
                setState(SystemState::WIFI_ERROR);
                break;

            case StateEventType::WIFI_PROVISION_REQUIRED:
                _wifiErr = true;
                setState(SystemState::WIFI_ERROR);
                break;

            case StateEventType::WIFI_PROVISION_DONE:
                setState(SystemState::WIFI_CONNECT);
                break;

            case StateEventType::TIME_SYNC_OK:
                _timeErr = false;
                xEventGroupSetBits(_eg, TIME_OK_BIT);
                if (!_manual) setState(SystemState::AUTO_MODE);
                break;

            case StateEventType::TIME_SYNC_FAIL:
                _timeErr = true;
                xEventGroupClearBits(_eg, TIME_OK_BIT);
                setState(SystemState::TIME_ERROR);
                break;

            case StateEventType::PULSE_UPDATE:
                setPulseInfo((uint16_t)ev.a, (uint32_t)ev.c, (uint8_t)ev.b, ev.flag, (uint32_t)time(nullptr));
                // Log pulse info every second
                //Serial.printf("Pulse count last min: %u, Mode: %s, Bypass: %s\n", _pulseCountLastMin, _manual ? "MAN" : "AUTO", _bypass ? "ON" : "OFF");
                if (_state == SystemState::PUMP_RUNNING && !_bypass && !ev.flag) {
                    _pumpErr = true;
                    xEventGroupSetBits(_eg, PUMP_ERROR_BIT);
                    stopPump();
                    setState(SystemState::PUMP_ERROR);
                    _log.add("PUMP_ERROR", "Pulse missing");
                }
                break;

            case StateEventType::PUMP_RUN_FINISHED:
                if (!_manual) setState(SystemState::AUTO_MODE);
                else setState(SystemState::MANUAL_MODE);
                break;

            case StateEventType::BUTTON_SHORT:
                if (_state == SystemState::WIFI_ERROR) {
                    setState(SystemState::WIFI_CONNECT);
                } else if (_state == SystemState::TIME_ERROR) {
                    setState(SystemState::TIME_SYNC);
                } else if (_state == SystemState::PUMP_ERROR) {
                    _pumpErr = false;
                    xEventGroupClearBits(_eg, PUMP_ERROR_BIT);
                    _manual = true;
                    updateBypass();
                    xEventGroupSetBits(_eg, MANUAL_MODE_BIT);
                    xEventGroupClearBits(_eg, AUTO_MODE_BIT);
                    setState(SystemState::MANUAL_MODE);
                } else if (_manual) {
                    if (_state == SystemState::PUMP_RUNNING) stopPump();
                    else startPumpManual();
                }
                break;

            case StateEventType::BUTTON_LONG:
                if (_state == SystemState::AUTO_MODE) {
                    _manual = true;
                    _bypassApiValue = false;
                    updateBypass();
                    xEventGroupSetBits(_eg, MANUAL_MODE_BIT);
                    xEventGroupClearBits(_eg, AUTO_MODE_BIT);
                    Serial.println("Switching to manual mode");
                    setState(SystemState::MANUAL_MODE);
                } else if (_state == SystemState::MANUAL_MODE) {
                    _manual = false;
                    _bypassApiValue = false;
                    updateBypass();
                    xEventGroupSetBits(_eg, AUTO_MODE_BIT);
                    xEventGroupClearBits(_eg, MANUAL_MODE_BIT | BYPASS_ACTIVE_BIT);
                    Serial.println("Switching to auto mode");
                    setState(SystemState::AUTO_MODE);
                }
                break;

            case StateEventType::BUTTON_VLONG:
                if (_state == SystemState::MANUAL_MODE) setState(SystemState::BYPASS_MODE);
                break;

            case StateEventType::BUTTON_RELEASE:
                if (_state == SystemState::BYPASS_MODE && ev.a >= 5000) {
                    _bypassApiValue = true;
                    updateBypass();
                    xEventGroupSetBits(_eg, BYPASS_ACTIVE_BIT);
                    setState(SystemState::MANUAL_MODE);
                    _log.add("BYPASS", "Enabled");
                }
                break;

            case StateEventType::API_SET_MODE_AUTO:
                _manual = false;
                updateBypass();
                xEventGroupSetBits(_eg, AUTO_MODE_BIT);
                xEventGroupClearBits(_eg, MANUAL_MODE_BIT | BYPASS_ACTIVE_BIT);
                setState(SystemState::AUTO_MODE);
                _log.add("MODE_CHANGE", "Auto mode via API");
                break;

            case StateEventType::API_SET_MODE_MANUAL:
                _manual = true;
                updateBypass();
                xEventGroupSetBits(_eg, MANUAL_MODE_BIT);
                xEventGroupClearBits(_eg, AUTO_MODE_BIT);
                setState(SystemState::MANUAL_MODE);
                _log.add("MODE_CHANGE", "Manual mode via API");
                break;

            case StateEventType::API_SET_BYPASS_ON:
                _bypassApiValue = true;
                updateBypass();
                xEventGroupSetBits(_eg, BYPASS_ACTIVE_BIT);
                _log.add("BYPASS", "Enabled via API");
                break;

            case StateEventType::API_SET_BYPASS_OFF:
                _bypassApiValue = false;
                updateBypass();
                xEventGroupClearBits(_eg, BYPASS_ACTIVE_BIT);
                _log.add("BYPASS", "Disabled via API");
                break;

            case StateEventType::API_PUMP_START:
                startPumpManual();
                _log.add("PUMP", "Started via API");
                break;

            case StateEventType::API_PUMP_STOP:
                stopPump();
                _log.add("PUMP", "Stopped via API");
                break;

            case StateEventType::API_SET_SCHEDULE:
                _schedule.startHour = (uint8_t)ev.a;
                _schedule.startMinute = (uint8_t)ev.b;
                _schedule.durationMinutes = (uint16_t)ev.c;
                _cfg.saveSchedule(_schedule);
                _log.add("SCHEDULE", "Updated");
                break;

            case StateEventType::API_RETRY_WIFI:
                setState(SystemState::WIFI_CONNECT);
                break;

            case StateEventType::API_RETRY_TIME:
                setState(SystemState::TIME_SYNC);
                break;

            case StateEventType::API_RESET_PUMP_ERROR:
                if (_state == SystemState::PUMP_ERROR) {
                    _pumpErr = false;
                    xEventGroupClearBits(_eg, PUMP_ERROR_BIT);
                    _manual = true;
                    updateBypass();
                    xEventGroupSetBits(_eg, MANUAL_MODE_BIT);
                    xEventGroupClearBits(_eg, AUTO_MODE_BIT);
                    setState(SystemState::MANUAL_MODE);
                    _log.add("PUMP_ERROR_RESET", "Via API");
                }
                break;

            default:
                break;
        }
    }

    if (_state == SystemState::AUTO_MODE) checkAutoSchedule();
}

void StateMachine::getSnapshotJson(String& outHeartbeat, String& outStatus) {
    JsonDocument hb;
    hb["timestamp"] = (uint32_t)time(nullptr);
    hb["state"] = stateToString(_state);
    hb["wifi_rssi"] = _wifiRssi;
    hb["uptime_sec"] = (millis() - _uptimeStart) / 1000U;
    hb["last_pulse_timestamp"] = _lastPulseTs;
    serializeJson(hb, outHeartbeat);

    JsonDocument st;
    st["state"] = stateToString(_state);
    st["mode"] = _manual ? "MANUAL" : "AUTO";
    st["bypass"] = _bypass;

    JsonObject errors = st["errors"].to<JsonObject>();
    errors["wifi"] = _wifiErr;
    errors["time"] = _timeErr;
    errors["pump"] = _pumpErr;

    JsonObject pump = st["pump"].to<JsonObject>();
    pump["running"] = (_state == SystemState::PUMP_RUNNING);
    pump["test_phase"] = false;
    pump["pulse_ok"] = _pulseOk;
    pump["pulse_frequency_hz"] = _pulseHz;
    pump["pulse_count_last_minute"] = _pulseCountLastMin;
    pump["pulse_stability"] = _pulseStability;

    JsonObject sch = st["schedule"].to<JsonObject>();
    sch["start_hour"] = _schedule.startHour;
    sch["start_minute"] = _schedule.startMinute;
    sch["duration_minutes"] = _schedule.durationMinutes;

    serializeJson(st, outStatus);
}