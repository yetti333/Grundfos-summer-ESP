#include <Arduino.h>
#include <esp_task_wdt.h>
#include "AppTypes.h"
#include "ConfigStorage.h"
#include "EventLog.h"
#include "LedController.h"
#include "ButtonHandler.h"
#include "PulseMonitor.h"
#include "WifiManager.h"
#include "TimeManager.h"
#include "PumpControl.h"
#include "StateMachine.h"
#include "RestApiServer.h"

QueueHandle_t qButtonEvents;
QueueHandle_t qPulseEvents;
QueueHandle_t qLedCommands;
QueueHandle_t qStateEvents;
QueueHandle_t qPumpCommands;
EventGroupHandle_t gEventGroup;

ConfigStorage gCfg;
EventLog gLog;
LedController* gLed = nullptr;
ButtonHandler* gButton = nullptr;
PulseMonitor* gPulse = nullptr;
WifiManager* gWifi = nullptr;
TimeManager* gTime = nullptr;
PumpControl* gPump = nullptr;
StateMachine* gSm = nullptr;
RestApiServer* gApi = nullptr;

bool detectBootResetRequest() {
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    uint32_t start = millis();
    while (millis() - start < 5000) {
        if (digitalRead(PIN_BUTTON) != LOW) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

void taskLED(void*) {
    esp_task_wdt_add(nullptr);
    for (;;) {
        gLed->taskLoop();
        esp_task_wdt_reset();
    }
}
void taskButton(void*) {
    esp_task_wdt_add(nullptr);
    for (;;) {
        gButton->taskLoop();
        esp_task_wdt_reset();
    }
}
void taskPulse(void*) {
    esp_task_wdt_add(nullptr);
    for (;;) {
        gPulse->taskLoop();
        esp_task_wdt_reset();
    }
}
void taskWifi(void*) {
    esp_task_wdt_add(nullptr);
    for (;;) {
        gWifi->taskLoop();
        esp_task_wdt_reset();
    }
}
void taskTime(void*) {
    esp_task_wdt_add(nullptr);
    for (;;) {
        gTime->taskLoop();
        esp_task_wdt_reset();
    }
}
void taskPump(void*) {
    esp_task_wdt_add(nullptr);
    for (;;) {
        gPump->taskLoop();
        esp_task_wdt_reset();
    }
}
void taskState(void*) {
    esp_task_wdt_add(nullptr);
    for (;;) {
        gSm->taskLoop();
        esp_task_wdt_reset();
    }
}
void taskApi(void*) {
    esp_task_wdt_add(nullptr);
    for (;;) {
        gApi->taskLoop();
        esp_task_wdt_reset();
    }
}

void setup() {
    Serial.begin(115200);
    esp_task_wdt_init(10, true);

    gCfg.begin();

    qButtonEvents = xQueueCreate(16, sizeof(ButtonEvent));
    qPulseEvents = xQueueCreate(16, sizeof(PulseEvent));
    qLedCommands = xQueueCreate(16, sizeof(LedPattern));
    qStateEvents = xQueueCreate(32, sizeof(StateEvent));
    qPumpCommands = xQueueCreate(8, sizeof(PumpCommand));
    gEventGroup = xEventGroupCreate();

    gLed = new LedController(qLedCommands);
    gButton = new ButtonHandler(qButtonEvents, qStateEvents);
    gPulse = new PulseMonitor(qPulseEvents, qStateEvents);
    gWifi = new WifiManager(qStateEvents, gCfg);
    gTime = new TimeManager(qStateEvents);
    gPump = new PumpControl(qPumpCommands, qStateEvents, gEventGroup);
    gSm = new StateMachine(qStateEvents, qLedCommands, qPumpCommands, gEventGroup, gCfg, gLog);
    gApi = new RestApiServer(qStateEvents, *gSm, gLog, gCfg);

    gLed->begin();
    gButton->begin();
    gPulse->begin();
    gPump->begin();
    gSm->begin();

    bool bootReset = detectBootResetRequest();
    if (bootReset) {
        LedPattern p = LedPattern::ALL_SOLID;
        xQueueSend(qLedCommands, &p, 0);
        while (digitalRead(PIN_BUTTON) == LOW) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        gCfg.clearAll();
        gLog.add("BOOT", "WiFi credentials erased");
    }

    gWifi->begin(bootReset);
    gTime->begin();
    gApi->begin();

    xTaskCreatePinnedToCore(taskLED, "TaskLED", 4096, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(taskButton, "TaskButton", 4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(taskPulse, "TaskPulseMonitor", 4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(taskWifi, "TaskWifiManager", 6144, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(taskTime, "TaskTimeManager", 4096, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(taskPump, "TaskPumpControl", 4096, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(taskState, "TaskStateMachine", 8192, nullptr, 4, nullptr, 1);
    xTaskCreatePinnedToCore(taskApi, "TaskRestApi", 8192, nullptr, 2, nullptr, 0);

    StateEvent startEv{StateEventType::API_RETRY_WIFI, 0, 0, 0, false};
    xQueueSend(qStateEvents, &startEv, 0);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}