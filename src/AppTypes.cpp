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

const char* pulseStateToString(PulseInterpretedState s) {
    switch (s) {
        case PulseInterpretedState::STANDBY: return "standby";
        case PulseInterpretedState::LOW_OPERATION: return "low_operation";
        case PulseInterpretedState::NORMAL_OPERATION: return "normal_operation";
        case PulseInterpretedState::ALARM_LOW_VOLTAGE: return "alarm_low_voltage";
        case PulseInterpretedState::ALARM_ROTOR_BLOCKED: return "alarm_rotor_blocked";
        case PulseInterpretedState::ALARM_ELECTRICAL_FAULT: return "alarm_electrical_fault";
        case PulseInterpretedState::INVALID_FREQUENCY: return "invalid_frequency";
        case PulseInterpretedState::PULSE_MISSING: return "pulse_missing";
        case PulseInterpretedState::UNKNOWN:
        default:
            return "unknown";
    }
}