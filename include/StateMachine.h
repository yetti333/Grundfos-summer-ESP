#pragma once

#include "AppTypes.h"
#include "EventLog.h"
#include "ConfigStorage.h"

class StateMachine {
public:
    StateMachine(QueueHandle_t stateQ, QueueHandle_t ledQ, QueueHandle_t pumpCmdQ, EventGroupHandle_t eg,
                 ConfigStorage& cfg, EventLog& log);

    void begin();
    void taskLoop();

    SystemState state() const;
    ScheduleConfig schedule() const;
    void getSnapshotJson(String& outHeartbeat, String& outStatus);
    bool isBypassActive() const { return _bypass; }

    void setWifiInfo(bool connected, int32_t rssi);
    void setPulseInfo(uint16_t hz, uint32_t count, uint8_t stability, bool ok, uint32_t lastTs,
                      uint32_t periodUs, uint32_t highUs, uint16_t dutyX100,
                      PulseInterpretedState interpreted, bool validFrequency);

private:
    void setState(SystemState s);
    void setLed(LedPattern p);
    void startPumpAuto();
    void startPumpManual();
    void stopPump();
    void checkAutoSchedule();
    void updateBypass();

    QueueHandle_t _stateQ;
    QueueHandle_t _ledQ;
    QueueHandle_t _pumpCmdQ;
    EventGroupHandle_t _eg;
    ConfigStorage& _cfg;
    EventLog& _log;
    SemaphoreHandle_t _mtx;

    SystemState _state;
    ScheduleConfig _schedule;
    bool _wifiErr;
    bool _timeErr;
    bool _pumpErr;
    bool _bypass;
    bool _bypassApiValue;  // API-controlled bypass value
    bool _manual;

    bool _wifiConnected;
    int32_t _wifiRssi;
    bool _pulseOk;
    uint16_t _pulseHz;
    uint32_t _pulseCountLastMin;
    uint8_t _pulseStability;
    uint32_t _lastPulseTs;
    uint32_t _pulsePeriodUs;
    uint32_t _pulseHighUs;
    uint16_t _pulseDutyX100;
    PulseInterpretedState _pulseInterpreted;
    bool _pulseValidFrequency;
    uint32_t _pumpRunStartMs;

    uint32_t _uptimeStart;
    int _lastAutoMinute;
};