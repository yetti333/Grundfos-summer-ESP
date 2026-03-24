#include "PumpControl.h"

PumpControl::PumpControl(QueueHandle_t cmdQ, QueueHandle_t stateQ, EventGroupHandle_t eg)
    : _cmdQ(cmdQ), _stateQ(stateQ), _eg(eg), _running(false), _startMs(0),
      _durationSec(0), _testSec(0) {}

void PumpControl::begin() {
    _relay.begin();
}

bool PumpControl::isRunning() const {
    return _running;
}

void PumpControl::taskLoop() {
    PumpCommand cmd;
    if (xQueueReceive(_cmdQ, &cmd, 0) == pdTRUE) {
        if (cmd.type == PumpCommandType::STOP) {
            _relay.set(false);
            _running = false;
            xEventGroupClearBits(_eg, PUMP_RUNNING_BIT);
        } else {
            _relay.set(true);
            _running = true;
            _startMs = millis();
            _durationSec = cmd.durationSec;
            _testSec = cmd.testSec;
            xEventGroupSetBits(_eg, PUMP_RUNNING_BIT);
        }
    }

    if (_running) {
        uint32_t elapsed = (millis() - _startMs) / 1000U;

        if (_durationSec > 0 && elapsed >= _durationSec) {
            _relay.set(false);
            _running = false;
            xEventGroupClearBits(_eg, PUMP_RUNNING_BIT);
            StateEvent ev{StateEventType::PUMP_RUN_FINISHED, 0, 0, 0, false};
            xQueueSend(_stateQ, &ev, 0);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}