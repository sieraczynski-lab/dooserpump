#include "config_manager.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

namespace ConfigManager {

// ============================================================
void begin() {
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS: błąd montowania – formatowanie...");
        LittleFS.format();
        if (!LittleFS.begin(true)) {
            Serial.println("LittleFS: krytyczny błąd!");
            return;
        }
    }
    Serial.printf("LittleFS: OK (wolne: %u B)\n", LittleFS.totalBytes() - LittleFS.usedBytes());
}

// ============================================================
bool load() {
    if (!LittleFS.exists(CONFIG_FILE)) {
        Serial.println("ConfigManager: brak pliku – używam domyślnych");
        // Ustaw domyślne nazwy pomp
        for (int i = 0; i < NUM_PUMPS; i++) {
            snprintf(pumpCal[i].name, sizeof(pumpCal[i].name), "Pompa %d", i + 1);
        }
        return false;
    }

    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("ConfigManager: błąd JSON: %s\n", err.c_str());
        return false;
    }

    // ── Sieć ────────────────────────────────────────────────
    JsonObject net = doc["network"];
    if (net) {
        strlcpy(netCfg.ssid,       net["ssid"]        | "",                sizeof(netCfg.ssid));
        strlcpy(netCfg.password,   net["pass"]        | "",                sizeof(netCfg.password));
        strlcpy(netCfg.staticIp,   net["ip"]          | "",                sizeof(netCfg.staticIp));
        strlcpy(netCfg.gateway,    net["gw"]          | "",                sizeof(netCfg.gateway));
        strlcpy(netCfg.subnet,     net["sn"]          | "255.255.255.0",   sizeof(netCfg.subnet));
        strlcpy(netCfg.mqttBroker, net["mqtt_host"]   | "",                sizeof(netCfg.mqttBroker));
        netCfg.mqttPort = net["mqtt_port"] | MQTT_PORT_DEFAULT;
        strlcpy(netCfg.mqttUser,   net["mqtt_user"]   | "",                sizeof(netCfg.mqttUser));
        strlcpy(netCfg.mqttPass,   net["mqtt_pass"]   | "",                sizeof(netCfg.mqttPass));
        strlcpy(netCfg.mqttTopic,  net["mqtt_topic"]  | MQTT_BASE_TOPIC,   sizeof(netCfg.mqttTopic));
        strlcpy(netCfg.webUser,    net["web_user"]    | "admin",           sizeof(netCfg.webUser));
        strlcpy(netCfg.webPass,    net["web_pass"]    | "admin",           sizeof(netCfg.webPass));
        netCfg.configured = net["configured"] | false;
    }

    // ── Ustawienia ogólne ────────────────────────────────────
    JsonObject gen = doc["general"];
    if (gen) {
        genCfg.pumpIntervalMin = gen["interval_min"] | DEFAULT_PUMP_INTERVAL_MIN;
        genCfg.ntpEnabled      = gen["ntp"]          | true;
    }

    // ── Kalibracja pomp ──────────────────────────────────────
    JsonArray pumps = doc["pumps"].as<JsonArray>();
    int pi = 0;
    for (JsonObject p : pumps) {
        if (pi >= NUM_PUMPS) break;
        pumpCal[pi].pulsesPerMl = p["ppm"]        | DEFAULT_PULSES_PER_ML;
        pumpCal[pi].mlPerDay    = p["ml_day"]     | DEFAULT_ML_PER_DAY;
        pumpCal[pi].dosesPerDay = p["doses"]      | DEFAULT_DAILY_DOSES;
        pumpCal[pi].enabled     = p["enabled"]    | true;
        pumpCal[pi].windowStart = p["win_start"]  | (uint8_t)0;
        pumpCal[pi].windowEnd   = p["win_end"]    | (uint8_t)24;
        const char* nm = p["name"] | "";
        if (strlen(nm) > 0) {
            strlcpy(pumpCal[pi].name, nm, sizeof(pumpCal[pi].name));
        } else {
            snprintf(pumpCal[pi].name, sizeof(pumpCal[pi].name), "Pompa %d", pi + 1);
        }
        pi++;
    }

    // ── Rekordy dawkowania ───────────────────────────────────
    JsonArray recs = doc["records"].as<JsonArray>();
    int ri = 0;
    for (JsonObject r : recs) {
        if (ri >= NUM_PUMPS) break;
        doseRec[ri].mlToday    = r["ml_today"]  | 0.0f;
        doseRec[ri].mlTotal    = r["ml_total"]  | 0.0f;
        doseRec[ri].pulsesTotal = r["pulses"]   | 0L;
        doseRec[ri].lastDose   = (time_t)(r["last_dose"] | 0);
        ri++;
    }

    Serial.println("ConfigManager: konfiguracja załadowana");
    return true;
}

// ============================================================
bool save() {
    JsonDocument doc;

    JsonObject net = doc["network"].to<JsonObject>();
    net["ssid"]       = netCfg.ssid;
    net["pass"]       = netCfg.password;
    net["ip"]         = netCfg.staticIp;
    net["gw"]         = netCfg.gateway;
    net["sn"]         = netCfg.subnet;
    net["mqtt_host"]  = netCfg.mqttBroker;
    net["mqtt_port"]  = netCfg.mqttPort;
    net["mqtt_user"]  = netCfg.mqttUser;
    net["mqtt_pass"]  = netCfg.mqttPass;
    net["mqtt_topic"] = netCfg.mqttTopic;
    net["web_user"]   = netCfg.webUser;
    net["web_pass"]   = netCfg.webPass;
    net["configured"] = netCfg.configured;

    JsonObject gen = doc["general"].to<JsonObject>();
    gen["interval_min"] = genCfg.pumpIntervalMin;
    gen["ntp"]          = genCfg.ntpEnabled;

    JsonArray pumps = doc["pumps"].to<JsonArray>();
    for (int i = 0; i < NUM_PUMPS; i++) {
        JsonObject p = pumps.add<JsonObject>();
        p["name"]      = pumpCal[i].name;
        p["ppm"]       = pumpCal[i].pulsesPerMl;
        p["ml_day"]    = pumpCal[i].mlPerDay;
        p["doses"]     = pumpCal[i].dosesPerDay;
        p["enabled"]   = pumpCal[i].enabled;
        p["win_start"] = pumpCal[i].windowStart;
        p["win_end"]   = pumpCal[i].windowEnd;
    }

    JsonArray recs = doc["records"].to<JsonArray>();
    for (int i = 0; i < NUM_PUMPS; i++) {
        JsonObject r = recs.add<JsonObject>();
        r["ml_today"]  = doseRec[i].mlToday;
        r["ml_total"]  = doseRec[i].mlTotal;
        r["pulses"]    = doseRec[i].pulsesTotal;
        r["last_dose"] = (long)doseRec[i].lastDose;
    }

    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) {
        Serial.println("ConfigManager: błąd otwierania pliku do zapisu");
        return false;
    }
    size_t written = serializeJson(doc, f);
    f.close();
    Serial.printf("ConfigManager: zapisano %u B\n", written);
    return written > 0;
}

// ============================================================
bool saveDoseRecords() {
    // Najprostsze: pełny zapis – dane są małe, nie ma problemu z wydajnością
    return save();
}

// ============================================================
void resetToDefaults() {
    netCfg = NetworkConfig{};
    genCfg = GeneralSettings{};
    for (int i = 0; i < NUM_PUMPS; i++) {
        pumpCal[i] = PumpCalibration{};
        doseRec[i] = DoseRecord{};
        snprintf(pumpCal[i].name, sizeof(pumpCal[i].name), "Pompa %d", i + 1);
    }
    if (LittleFS.exists(CONFIG_FILE)) LittleFS.remove(CONFIG_FILE);
    Serial.println("ConfigManager: reset do domyślnych");
}

// ============================================================
String getConfigJson() {
    JsonDocument doc;

    JsonObject net = doc["network"].to<JsonObject>();
    net["ssid"]       = netCfg.ssid;
    net["ip"]         = netCfg.staticIp;
    net["gw"]         = netCfg.gateway;
    net["sn"]         = netCfg.subnet;
    net["mqtt_host"]  = netCfg.mqttBroker;
    net["mqtt_port"]  = netCfg.mqttPort;
    net["mqtt_topic"] = netCfg.mqttTopic;
    net["web_user"]   = netCfg.webUser;
    net["configured"] = netCfg.configured;
    // Hasła celowo pominięte w GET

    JsonObject gen = doc["general"].to<JsonObject>();
    gen["interval_min"] = genCfg.pumpIntervalMin;
    gen["ntp"]          = genCfg.ntpEnabled;

    JsonArray pumps = doc["pumps"].to<JsonArray>();
    for (int i = 0; i < NUM_PUMPS; i++) {
        JsonObject p = pumps.add<JsonObject>();
        p["idx"]       = i;
        p["name"]      = pumpCal[i].name;
        p["ppm"]       = pumpCal[i].pulsesPerMl;
        p["ml_day"]    = pumpCal[i].mlPerDay;
        p["doses"]     = pumpCal[i].dosesPerDay;
        p["enabled"]   = pumpCal[i].enabled;
        p["win_start"] = pumpCal[i].windowStart;
        p["win_end"]   = pumpCal[i].windowEnd;
        p["ml_today"]  = doseRec[i].mlToday;
        p["ml_total"]  = doseRec[i].mlTotal;
    }

    String out;
    serializeJson(doc, out);
    return out;
}

// ============================================================
bool applyConfigJson(const String& json) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

    JsonObject net = doc["network"];
    if (net) {
        if (!net["ssid"].isNull())
            strlcpy(netCfg.ssid,       net["ssid"],        sizeof(netCfg.ssid));
        // Hasło aktualizuj tylko gdy niepuste
        const char* pw = net["pass"] | "";
        if (strlen(pw) > 0)
            strlcpy(netCfg.password,   pw,                 sizeof(netCfg.password));
        if (!net["ip"].isNull())
            strlcpy(netCfg.staticIp,   net["ip"],          sizeof(netCfg.staticIp));
        if (!net["gw"].isNull())
            strlcpy(netCfg.gateway,    net["gw"],          sizeof(netCfg.gateway));
        if (!net["mqtt_host"].isNull())
            strlcpy(netCfg.mqttBroker, net["mqtt_host"],   sizeof(netCfg.mqttBroker));
        if (!net["mqtt_port"].isNull())
            netCfg.mqttPort = net["mqtt_port"];
        if (!net["mqtt_user"].isNull())
            strlcpy(netCfg.mqttUser,   net["mqtt_user"],   sizeof(netCfg.mqttUser));
        const char* mpw = net["mqtt_pass"] | "";
        if (strlen(mpw) > 0)
            strlcpy(netCfg.mqttPass,   mpw,                sizeof(netCfg.mqttPass));
        if (!net["mqtt_topic"].isNull())
            strlcpy(netCfg.mqttTopic,  net["mqtt_topic"],  sizeof(netCfg.mqttTopic));
        if (!net["web_user"].isNull())
            strlcpy(netCfg.webUser,    net["web_user"],    sizeof(netCfg.webUser));
        const char* wpw = net["web_pass"] | "";
        if (strlen(wpw) > 0)
            strlcpy(netCfg.webPass,    wpw,                sizeof(netCfg.webPass));
        if (!net["configured"].isNull())
            netCfg.configured = net["configured"];
    }

    JsonObject gen = doc["general"];
    if (gen) {
        if (!gen["interval_min"].isNull()) genCfg.pumpIntervalMin = gen["interval_min"];
        if (!gen["ntp"].isNull())          genCfg.ntpEnabled       = gen["ntp"];
    }

    JsonArray pumps = doc["pumps"].as<JsonArray>();
    for (JsonObject p : pumps) {
        int idx = p["idx"] | -1;
        if (idx < 0 || idx >= NUM_PUMPS) continue;
        const char* nm = p["name"] | "";
        if (strlen(nm) > 0) strlcpy(pumpCal[idx].name, nm, sizeof(pumpCal[idx].name));
        if (!p["ppm"].isNull())       pumpCal[idx].pulsesPerMl = p["ppm"];
        if (!p["ml_day"].isNull())    pumpCal[idx].mlPerDay    = p["ml_day"];
        if (!p["doses"].isNull())     pumpCal[idx].dosesPerDay = max(1, (int)p["doses"]);
        if (!p["enabled"].isNull())   pumpCal[idx].enabled     = p["enabled"];
        if (!p["win_start"].isNull()) pumpCal[idx].windowStart = (uint8_t)(int)p["win_start"];
        if (!p["win_end"].isNull())   pumpCal[idx].windowEnd   = (uint8_t)(int)p["win_end"];
    }

    return save();
}

} // namespace ConfigManager
