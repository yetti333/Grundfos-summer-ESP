#include "AppTypes.h"

const char* stateToString(SystemState s) {
    switch (s) {
        case SystemState::BOOT: return "BOOT";
        case SystemState::WIFI_CONNECT: return "WIFI_CONNECT";
        case SystemState::WIFI_ERROR: return "WIFI_ERROR";
        case SystemState::TIME_SYNC: return "TIME_SYNC";
        case SystemState::TIME_ERROR: return "TIME_ERROR";
        case SystemState::AUTO_MODE: return "AUTO_MODE";
        case SystemState::MANUAL_MODE: return "MANUAL_MODE";
        case SystemState::BYPASS_MODE: return "BYPASS_MODE";
        case SystemState::PUMP_RUNNING: return "PUMP_RUNNING";
        case SystemState::PUMP_ERROR: return "PUMP_ERROR";
        default: return "UNKNOWN";
    }
}