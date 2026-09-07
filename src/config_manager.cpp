#include "config_manager.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

namespace ConfigManager {

// Ustawienia (sieć/MQTT/kalibracja/rekordy) żyją w NVS, NIE w LittleFS.
// Powód: `pio run -t uploadfs` kasuje CAŁĄ partycję LittleFS i zastępuje ją
// obrazem zbudowanym wyłącznie z lokalnego folderu data/ (tam jest tylko
// index.html) – każdy plik zapisywany w runtime na LittleFS ginie bezpowrotnie
// przy najbliższym uploadfs. NVS to osobna partycja (patrz partitions_4mb.csv),
// której nie rusza ani `upload`, ani `uploadfs`.
static Preferences _prefs;
static constexpr const char* NVS_NAMESPACE = "cfg";

static void _getStr(const char* key, char* dst, size_t dstSize) {
    String s = _prefs.getString(key, dst); // dst już zawiera wartość domyślną
    strlcpy(dst, s.c_str(), dstSize);
}

// ============================================================
void begin() {
    // LittleFS służy już tylko do serwowania panelu WWW (data/index.html)
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
    // Domyślne nazwy pomp – ustaw PRZED odczytem (nadpisuje bezużyteczne
    // "Pompa" bez numeru z domyślnej wartości pola w strukturze); odczyt
    // i tak użyje tego tylko jako fallback gdy klucz nie istnieje w NVS.
    for (int i = 0; i < NUM_PUMPS; i++) {
        snprintf(pumpCal[i].name, sizeof(pumpCal[i].name), "Pompa %d", i + 1);
    }

    _prefs.begin(NVS_NAMESPACE, true); // read-only
    bool hadData = _prefs.isKey("w_configured");

    // ── Sieć ────────────────────────────────────────────────
    _getStr("w_ssid",   netCfg.ssid,       sizeof(netCfg.ssid));
    _getStr("w_pass",   netCfg.password,   sizeof(netCfg.password));
    _getStr("w_ip",     netCfg.staticIp,   sizeof(netCfg.staticIp));
    _getStr("w_gw",     netCfg.gateway,    sizeof(netCfg.gateway));
    _getStr("w_sn",     netCfg.subnet,     sizeof(netCfg.subnet));
    _getStr("w_mhost",  netCfg.mqttBroker, sizeof(netCfg.mqttBroker));
    netCfg.mqttPort = _prefs.getUShort("w_mport", netCfg.mqttPort);
    _getStr("w_muser",  netCfg.mqttUser,   sizeof(netCfg.mqttUser));
    _getStr("w_mpass",  netCfg.mqttPass,   sizeof(netCfg.mqttPass));
    _getStr("w_mtopic", netCfg.mqttTopic,  sizeof(netCfg.mqttTopic));
    _getStr("w_wuser",  netCfg.webUser,    sizeof(netCfg.webUser));
    _getStr("w_wpass",  netCfg.webPass,    sizeof(netCfg.webPass));
    netCfg.configured = _prefs.getBool("w_configured", netCfg.configured);

    // ── Ustawienia ogólne ────────────────────────────────────
    genCfg.pumpIntervalMin = _prefs.getUShort("g_interval", genCfg.pumpIntervalMin);
    genCfg.ntpEnabled      = _prefs.getBool("g_ntp", genCfg.ntpEnabled);

    // ── Kalibracja pomp + rekordy dawkowania ─────────────────
    char key[16];
    for (int i = 0; i < NUM_PUMPS; i++) {
        snprintf(key, sizeof(key), "p%d_ppm", i);
        pumpCal[i].pulsesPerMl = _prefs.getFloat(key, pumpCal[i].pulsesPerMl);
        snprintf(key, sizeof(key), "p%d_mld", i);
        pumpCal[i].mlPerDay = _prefs.getFloat(key, pumpCal[i].mlPerDay);
        snprintf(key, sizeof(key), "p%d_doses", i);
        pumpCal[i].dosesPerDay = _prefs.getInt(key, pumpCal[i].dosesPerDay);
        snprintf(key, sizeof(key), "p%d_en", i);
        pumpCal[i].enabled = _prefs.getBool(key, pumpCal[i].enabled);
        snprintf(key, sizeof(key), "p%d_ws", i);
        pumpCal[i].windowStart = (uint8_t)_prefs.getUChar(key, pumpCal[i].windowStart);
        snprintf(key, sizeof(key), "p%d_we", i);
        pumpCal[i].windowEnd = (uint8_t)_prefs.getUChar(key, pumpCal[i].windowEnd);
        snprintf(key, sizeof(key), "p%d_name", i);
        _getStr(key, pumpCal[i].name, sizeof(pumpCal[i].name));

        snprintf(key, sizeof(key), "r%d_today", i);
        doseRec[i].mlToday = _prefs.getFloat(key, doseRec[i].mlToday);
        snprintf(key, sizeof(key), "r%d_total", i);
        doseRec[i].mlTotal = _prefs.getFloat(key, doseRec[i].mlTotal);
        snprintf(key, sizeof(key), "r%d_pulses", i);
        doseRec[i].pulsesTotal = _prefs.getLong(key, doseRec[i].pulsesTotal);
        snprintf(key, sizeof(key), "r%d_last", i);
        doseRec[i].lastDose = (time_t)_prefs.getLong(key, (long)doseRec[i].lastDose);
    }

    _prefs.end();

    if (!hadData) {
        Serial.println("ConfigManager: brak zapisanej konfiguracji w NVS – używam domyślnych");
        return false;
    }
    Serial.println("ConfigManager: konfiguracja załadowana z NVS");
    return true;
}

// ============================================================
bool save() {
    _prefs.begin(NVS_NAMESPACE, false);

    _prefs.putString("w_ssid",   netCfg.ssid);
    _prefs.putString("w_pass",   netCfg.password);
    _prefs.putString("w_ip",     netCfg.staticIp);
    _prefs.putString("w_gw",     netCfg.gateway);
    _prefs.putString("w_sn",     netCfg.subnet);
    _prefs.putString("w_mhost",  netCfg.mqttBroker);
    _prefs.putUShort("w_mport",  netCfg.mqttPort);
    _prefs.putString("w_muser",  netCfg.mqttUser);
    _prefs.putString("w_mpass",  netCfg.mqttPass);
    _prefs.putString("w_mtopic", netCfg.mqttTopic);
    _prefs.putString("w_wuser",  netCfg.webUser);
    _prefs.putString("w_wpass",  netCfg.webPass);
    _prefs.putBool("w_configured", netCfg.configured);

    _prefs.putUShort("g_interval", genCfg.pumpIntervalMin);
    _prefs.putBool("g_ntp",        genCfg.ntpEnabled);

    char key[16];
    for (int i = 0; i < NUM_PUMPS; i++) {
        snprintf(key, sizeof(key), "p%d_ppm", i);   _prefs.putFloat(key, pumpCal[i].pulsesPerMl);
        snprintf(key, sizeof(key), "p%d_mld", i);   _prefs.putFloat(key, pumpCal[i].mlPerDay);
        snprintf(key, sizeof(key), "p%d_doses", i); _prefs.putInt(key, pumpCal[i].dosesPerDay);
        snprintf(key, sizeof(key), "p%d_en", i);    _prefs.putBool(key, pumpCal[i].enabled);
        snprintf(key, sizeof(key), "p%d_ws", i);    _prefs.putUChar(key, pumpCal[i].windowStart);
        snprintf(key, sizeof(key), "p%d_we", i);    _prefs.putUChar(key, pumpCal[i].windowEnd);
        snprintf(key, sizeof(key), "p%d_name", i);  _prefs.putString(key, pumpCal[i].name);

        snprintf(key, sizeof(key), "r%d_today", i);  _prefs.putFloat(key, doseRec[i].mlToday);
        snprintf(key, sizeof(key), "r%d_total", i);  _prefs.putFloat(key, doseRec[i].mlTotal);
        snprintf(key, sizeof(key), "r%d_pulses", i); _prefs.putLong(key, doseRec[i].pulsesTotal);
        snprintf(key, sizeof(key), "r%d_last", i);   _prefs.putLong(key, (long)doseRec[i].lastDose);
    }

    _prefs.end();
    Serial.println("ConfigManager: zapisano do NVS");
    return true;
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
    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.clear();
    _prefs.end();
    Serial.println("ConfigManager: reset do domyślnych (NVS wyczyszczone)");
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
        if (!net["sn"].isNull())
            strlcpy(netCfg.subnet,     net["sn"],          sizeof(netCfg.subnet));
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
