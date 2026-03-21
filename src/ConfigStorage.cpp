#include "ConfigStorage.h"

bool ConfigStorage::begin() {
    return _prefs.begin("grundfos", false);
}

void ConfigStorage::end() {
    _prefs.end();
}

bool ConfigStorage::loadSchedule(ScheduleConfig& cfg) {
    cfg.startHour = _prefs.getUChar("sch_h", 19);
    cfg.startMinute = _prefs.getUChar("sch_m", 0);
    cfg.durationMinutes = _prefs.getUShort("sch_d", 5);
    return true;
}

bool ConfigStorage::saveSchedule(const ScheduleConfig& cfg) {
    _prefs.putUChar("sch_h", cfg.startHour);
    _prefs.putUChar("sch_m", cfg.startMinute);
    _prefs.putUShort("sch_d", cfg.durationMinutes);
    return true;
}

bool ConfigStorage::loadWifi(String& ssid, String& password) {
    ssid = _prefs.getString("wifi_ssid", "");
    password = _prefs.getString("wifi_pass", "");
    return !ssid.isEmpty();
}

bool ConfigStorage::saveWifi(const String& ssid, const String& password) {
    _prefs.putString("wifi_ssid", ssid);
    _prefs.putString("wifi_pass", password);
    return true;
}

bool ConfigStorage::clearAll() {
    return _prefs.clear();
}