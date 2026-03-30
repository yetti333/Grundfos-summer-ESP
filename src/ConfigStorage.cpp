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
    Serial.printf("[CFG] loadWifi: got SSID='%s' (len=%d), PASS len=%d from Preferences\n",
                  ssid.c_str(), ssid.length(), password.length());
    if (ssid.isEmpty()) {
        Serial.println("[CFG] SSID is empty - returning false");
        return false;
    }
    return true;
}

bool ConfigStorage::saveWifi(const String& ssid, const String& password) {
    Serial.printf("[CFG] saveWifi: saving SSID='%s' (len=%d), PASS len=%d\n",
                  ssid.c_str(), ssid.length(), password.length());
    bool s1 = _prefs.putString("wifi_ssid", ssid);
    bool s2 = _prefs.putString("wifi_pass", password);
    if (s1 && s2) {
        Serial.println("[CFG] saveWifi: SUCCESS");
        return true;
    } else {
        Serial.printf("[CFG] saveWifi: FAILED (ssid=%d, pass=%d)\n", s1, s2);
        return false;
    }
}

bool ConfigStorage::clearAll() {
    return _prefs.clear();
}