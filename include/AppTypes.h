#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

constexpr gpio_num_t PIN_LED_GREEN = GPIO_NUM_21;
constexpr gpio_num_t PIN_LED_RED = GPIO_NUM_18;
constexpr gpio_num_t PIN_LED_BLUE = GPIO_NUM_19;
constexpr gpio_num_t PIN_BUTTON = GPIO_NUM_14;
constexpr gpio_num_t PIN_RELAY = GPIO_NUM_13;
constexpr gpio_num_t PIN_PULSE = GPIO_NUM_27;

constexpr EventBits_t WIFI_OK_BIT      = BIT0;
constexpr EventBits_t TIME_OK_BIT      = BIT1;
constexpr EventBits_t PUMP_RUNNING_BIT = BIT2;
constexpr EventBits_t PUMP_ERROR_BIT   = BIT3;
constexpr EventBits_t BYPASS_ACTIVE_BIT= BIT4;
constexpr EventBits_t MANUAL_MODE_BIT  = BIT5;
constexpr EventBits_t AUTO_MODE_BIT    = BIT6;

enum class SystemState : uint8_t {
    BOOT,
    WIFI_CONNECT,
    WIFI_ERROR,
    TIME_SYNC,
    TIME_ERROR,
    AUTO_MODE,
    MANUAL_MODE,
    BYPASS_MODE,
    PUMP_RUNNING,
    PUMP_ERROR
};

enum class LedPattern : uint8_t {
    OFF,
    ALL_SOLID,
    WIFI_CONNECT,
    WIFI_ERROR,
    TIME_ERROR,
    AUTO_IDLE,
    MANUAL_IDLE,
    BYPASS_IDLE,
    PUMP_RUNNING,
    PUMP_ERROR
};

enum class ButtonEventType : uint8_t {
    SHORT_PRESS,
    LONG_PRESS,
    VERY_LONG_PRESS,
    RELEASE
};

struct ButtonEvent {
    ButtonEventType type;
    uint32_t durationMs;
};

struct PulseEvent {
    uint32_t countLastSecond;
    uint16_t frequencyHz;
    uint8_t stabilityPercent;
    bool pulseOk;
    uint32_t lastPulseUnix;
};

enum class StateEventType : uint8_t {
    BUTTON_SHORT,
    BUTTON_LONG,
    BUTTON_VLONG,
    BUTTON_RELEASE,
    WIFI_CONNECTED,
    WIFI_DISCONNECTED,
    WIFI_PROVISION_REQUIRED,
    TIME_SYNC_OK,
    TIME_SYNC_FAIL,
    PULSE_UPDATE,
    PUMP_RUN_FINISHED,
    API_SET_MODE_AUTO,
    API_SET_MODE_MANUAL,
    API_SET_BYPASS_ON,
    API_SET_BYPASS_OFF,
    API_PUMP_START,
    API_PUMP_STOP,
    API_SET_SCHEDULE,
    API_RETRY_WIFI,
    API_RETRY_TIME
};

struct StateEvent {
    StateEventType type;
    int32_t a;
    int32_t b;
    int32_t c;
    bool flag;
};

struct ScheduleConfig {
    uint8_t startHour = 19;
    uint8_t startMinute = 0;
    uint16_t durationMinutes = 5;
};

enum class PumpCommandType : uint8_t {
    START_AUTO,
    START_MANUAL,
    STOP
};

struct PumpCommand {
    PumpCommandType type;
    uint32_t durationSec;
    uint32_t testSec;
};

const char* stateToString(SystemState s);