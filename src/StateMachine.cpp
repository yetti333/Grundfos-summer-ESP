#include "StateMachine.h"
#include <ArduinoJson.h>
#include <time.h>
#include <WiFi.h>

namespace {
String formatUptime(uint32_t uptimeSeconds) {
    const uint32_t days = uptimeSeconds / 86400U;
    const uint32_t hours = (uptimeSeconds % 86400U) / 3600U;
    const uint32_t minutes = (uptimeSeconds % 3600U) / 60U;
    return String(days) + "d " + String(hours) + "h " + String(minutes) + "m";
}

String isoNow() {
    time_t now = time(nullptr);
    struct tm t;
    if (localtime_r(&now, &t) == nullptr || t.tm_year < 124) {
        return "1970-01-01T00:00:00";
    }
    char out[24];
    strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%S", &t);
    return String(out);
}
}

StateMachine::StateMachine(QueueHandle_t stateQ, QueueHandle_t ledQ, QueueHandle_t pumpCmdQ, EventGroupHandle_t eg,
                           ConfigStorage& cfg, EventLog& log)
    : _stateQ(stateQ), _ledQ(ledQ), _pumpCmdQ(pumpCmdQ), _eg(eg), _cfg(cfg), _log(log),
      _state(SystemState::BOOT), _wifiErr(false), _timeErr(false), _pumpErr(false),
      _bypass(false), _bypassApiValue(false), _manual(false), _wifiConnected(false), _wifiRssi(-127), _pulseOk(false),
            _pulseHz(0), _pulseCountLastMin(0), _pulseStability(0), _lastPulseTs(0),
            _pulsePeriodUs(0), _pulseHighUs(0), _pulseDutyX100(0), _pulseInterpreted(PulseInterpretedState::UNKNOWN),
            _pulseValidFrequency(false), _pumpRunStartMs(0), _uptimeStart(millis()),
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

void StateMachine::setPulseInfo(uint16_t hz, uint32_t count, uint8_t stability, bool ok, uint32_t lastTs,
                                uint32_t periodUs, uint32_t highUs, uint16_t dutyX100,
                                PulseInterpretedState interpreted, bool validFrequency) {
    if (xSemaphoreTake(_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        _pulseHz = hz;
        _pulseCountLastMin = count * 60;
        _pulseStability = stability;
        _pulseOk = ok;
        if (lastTs > 0) _lastPulseTs = lastTs;
        _pulsePeriodUs = periodUs;
        _pulseHighUs = highUs;
        _pulseDutyX100 = dutyX100;
        _pulseInterpreted = interpreted;
        _pulseValidFrequency = validFrequency;
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
    _pumpRunStartMs = millis();
    setState(SystemState::PUMP_RUNNING);
    _log.add("PUMP_START", "Automatic schedule");
}

void StateMachine::startPumpManual() {
    PumpCommand c{PumpCommandType::START_MANUAL, 0, 0};
    xQueueSend(_pumpCmdQ, &c, 0);
    _pumpRunStartMs = millis();
    setState(SystemState::PUMP_RUNNING);
    _log.add("PUMP_START", "Manual");
}

void StateMachine::stopPump() {
    PumpCommand c{PumpCommandType::STOP, 0, 0};
    xQueueSend(_pumpCmdQ, &c, 0);
    _pumpRunStartMs = 0;
    if (_manual) setState(SystemState::MANUAL_MODE);
    else setState(SystemState::AUTO_MODE);
    _log.add("PUMP_STOP", "Requested");
}

void StateMachine::updateBypass() {
    _bypass = _bypassApiValue;

    if (_bypass) xEventGroupSetBits(_eg, BYPASS_ACTIVE_BIT);
    else xEventGroupClearBits(_eg, BYPASS_ACTIVE_BIT);
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
                {
                    const uint16_t hz = (uint16_t)(ev.a > 0 ? ev.a : 0);
                    const int err = abs((int)hz - 75);
                    int st = 100 - (err * 100 / 75);
                    if (st < 0) st = 0;
                    if (st > 100) st = 100;

                    setPulseInfo(hz,
                                 (uint32_t)hz,
                                 (uint8_t)st,
                                 ev.flag,
                                 ev.ts,
                                 (uint32_t)(ev.c > 0 ? ev.c : 0),
                                 (uint32_t)(ev.d > 0 ? ev.d : 0),
                                 (uint16_t)(ev.b > 0 ? ev.b : 0),
                                 (PulseInterpretedState)ev.e,
                                 ev.flag2);
                }

                if (_state == SystemState::PUMP_RUNNING && !_bypass) {
                    const PulseInterpretedState interpreted = (PulseInterpretedState)ev.e;
                    const bool startupGraceActive = _pumpRunStartMs > 0 && (millis() - _pumpRunStartMs) < 10000UL;

                    bool stopForPwmError = false;
                    const char* reason = nullptr;

                    if (interpreted == PulseInterpretedState::ALARM_LOW_VOLTAGE) {
                        stopForPwmError = true;
                        reason = "PWM alarm: low voltage";
                    } else if (interpreted == PulseInterpretedState::ALARM_ROTOR_BLOCKED) {
                        stopForPwmError = true;
                        reason = "PWM alarm: rotor blocked";
                    } else if (interpreted == PulseInterpretedState::ALARM_ELECTRICAL_FAULT) {
                        stopForPwmError = true;
                        reason = "PWM alarm: electrical fault";
                    } else if (interpreted == PulseInterpretedState::PULSE_MISSING && !startupGraceActive) {
                        stopForPwmError = true;
                        reason = "Pulse missing";
                    }

                    if (stopForPwmError) {
                        _pumpErr = true;
                        xEventGroupSetBits(_eg, PUMP_ERROR_BIT);
                        stopPump();
                        setState(SystemState::PUMP_ERROR);
                        _log.add("PUMP_ERROR", reason);
                    }
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
                    _bypassApiValue = true;
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
                    _bypassApiValue = true;
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
                    setState(SystemState::MANUAL_MODE);
                    _log.add("BYPASS", "Enabled");
                }
                break;

            case StateEventType::API_SET_MODE_AUTO:
                _manual = false;
                _bypassApiValue = false;
                updateBypass();
                xEventGroupSetBits(_eg, AUTO_MODE_BIT);
                xEventGroupClearBits(_eg, MANUAL_MODE_BIT);
                setState(SystemState::AUTO_MODE);
                _log.add("MODE_CHANGE", "Auto mode via API");
                break;

            case StateEventType::API_SET_MODE_MANUAL:
                _manual = true;
                _bypassApiValue = true;
                updateBypass();
                xEventGroupSetBits(_eg, MANUAL_MODE_BIT);
                xEventGroupClearBits(_eg, AUTO_MODE_BIT);
                setState(SystemState::MANUAL_MODE);
                _log.add("MODE_CHANGE", "Manual mode via API");
                break;

            case StateEventType::API_SET_BYPASS_ON:
                _bypassApiValue = true;
                updateBypass();
                _log.add("BYPASS", "Enabled via API");
                break;

            case StateEventType::API_SET_BYPASS_OFF:
                _bypassApiValue = false;
                updateBypass();
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
                    _bypassApiValue = true;
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
    const wifi_mode_t wifiMode = WiFi.getMode();
    const bool apMode = (wifiMode == WIFI_AP || wifiMode == WIFI_AP_STA);
    const bool stationMode = (wifiMode == WIFI_STA || wifiMode == WIFI_AP_STA);
    const bool connected = (WiFi.status() == WL_CONNECTED);
    const bool provisioningRequired = apMode && !connected;

    String currentIp = (stationMode && connected) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    if (currentIp.isEmpty()) {
        currentIp = "0.0.0.0";
    }

    const String mac = WiFi.macAddress();
    const String ssid = connected ? WiFi.SSID() : String("");
    const int32_t rssi = connected ? WiFi.RSSI() : -127;
    const uint32_t uptimeSeconds = (millis() - _uptimeStart) / 1000U;
    const String uptimeText = formatUptime(uptimeSeconds);
    const String nowIso = isoNow();

    JsonDocument hb;
    hb["timestamp"] = (uint32_t)time(nullptr);
    hb["state"] = stateToString(_state);
    hb["wifi_rssi"] = _wifiRssi;
    hb["uptime_sec"] = uptimeSeconds;
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
    pump["period_ms"] = _pulsePeriodUs / 1000.0f;
    pump["high_ms"] = _pulseHighUs / 1000.0f;
    pump["duty_percent"] = _pulseDutyX100 / 100.0f;
    pump["interpreted_state"] = pulseStateToString(_pulseInterpreted);
    pump["valid_frequency"] = _pulseValidFrequency;
    pump["timestamp"] = _lastPulseTs;

    JsonObject sch = st["schedule"].to<JsonObject>();
    sch["start_hour"] = _schedule.startHour;
    sch["start_minute"] = _schedule.startMinute;
    sch["duration_minutes"] = _schedule.durationMinutes;

    st["ip"] = currentIp;
    st["mac"] = mac;
    st["ssid"] = ssid;
    st["hostname"] = MDNS_HOSTNAME;
    st["mdns"] = MDNS_FQDN;
    st["rssi"] = rssi;

    JsonObject networkInfo = st["network_info"].to<JsonObject>();
    networkInfo["ip"] = currentIp;
    networkInfo["mac"] = mac;
    networkInfo["ssid"] = ssid;
    networkInfo["hostname"] = MDNS_HOSTNAME;
    networkInfo["mdns"] = MDNS_FQDN;
    networkInfo["rssi"] = rssi;
    networkInfo["ap_mode"] = apMode;
    networkInfo["station_mode"] = stationMode;
    networkInfo["connected"] = connected;
    networkInfo["last_seen"] = nowIso;

    JsonObject deviceInfo = st["device_info"].to<JsonObject>();
    deviceInfo["hostname"] = MDNS_HOSTNAME;
    deviceInfo["mdns"] = MDNS_FQDN;
    deviceInfo["firmware_version"] = "1.0.0";
    deviceInfo["uptime"] = uptimeText;
    deviceInfo["uptime_seconds"] = uptimeSeconds;
    deviceInfo["provisioning_required"] = provisioningRequired;

    st["uptime"] = uptimeText;
    st["uptime_seconds"] = uptimeSeconds;
    st["firmware_version"] = "1.0.0";
    st["ap_mode"] = apMode;
    st["station_mode"] = stationMode;
    st["provisioning_required"] = provisioningRequired;

    Serial.printf("[HTTP] GET /status -> ip=%s ap=%d\n", currentIp.c_str(), apMode ? 1 : 0);

    serializeJson(st, outStatus);
}