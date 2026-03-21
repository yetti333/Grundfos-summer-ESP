#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "AppTypes.h"

class ConfigStorage {
public:
    bool begin();
    void end();

    bool loadSchedule(ScheduleConfig& cfg);
    bool saveSchedule(const ScheduleConfig& cfg);

    bool loadWifi(String& ssid, String& password);
    bool saveWifi(const String& ssid, const String& password);

    bool clearAll();

private:
    Preferences _prefs;
};