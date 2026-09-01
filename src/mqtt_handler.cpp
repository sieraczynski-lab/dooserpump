#include "mqtt_handler.h"
#include "web_server_handler.h"
#include "config.h"
#include "config_manager.h"
#include "pump_controller.h"

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

namespace MqttHandler {

// forward declaration
void publishPump(uint8_t idx);

static WiFiClient   _wifiClient;
static PubSubClient _mqtt(_wifiClient);

// ── Pomocnik: buduj temat ────────────────────────────────────
static String _topic(const char* suffix) {
    return String(netCfg.mqttTopic) + suffix;
}

// ── Pomocnik: temat Discovery HA ─────────────────────────────
static String _disc(const char* component, const char* pumpId, const char* objectId) {
    return String(MQTT_HA_PREFIX) + "/" + component + "/" + pumpId + "/" + objectId + "/config";
}

// ── Callback: odebrana wiadomość ─────────────────────────────
static void _onMessage(char* topic, uint8_t* payload, unsigned int len) {
    if (len == 0 || len > 256) return;

    // Bezpieczna kopia payloadu
    char buf[257];
    memcpy(buf, payload, len);
    buf[len] = '\0';

    Serial.printf("MQTT rx [%s]: %s\n", topic, buf);
    WebServerHandler::wsLogf("MQTT rx [%s]: %s", topic, buf);

    // Sprawdź czy to temat komendy dla konkretnej pompy
    // Format: {base}/pump/{idx}/cmd
    String topicStr(topic);
    for (int i = 0; i < NUM_PUMPS; i++) {
        String cmdTopic = _topic("/pump/") + i + "/cmd";
        if (topicStr != cmdTopic) continue;

        JsonDocument doc;
        if (deserializeJson(doc, buf) != DeserializationError::Ok) return;

        const char* action = doc["action"] | "";
        if (strcmp(action, "dose") == 0) {
            float ml = doc["ml"] | 0.0f;
            if (ml > 0.0f && pumpCal[i].enabled) {
                PumpController::dose((uint8_t)i, ml);
            }
        } else if (strcmp(action, "stop") == 0) {
            PumpController::stop((uint8_t)i);
        } else if (strcmp(action, "toggle") == 0) {
            pumpCal[i].enabled = !pumpCal[i].enabled;
            ConfigManager::save();
            publishPump((uint8_t)i);
        } else if (strcmp(action, "enable") == 0) {
            pumpCal[i].enabled = true;
            ConfigManager::save();
            publishPump((uint8_t)i);
        } else if (strcmp(action, "disable") == 0) {
            pumpCal[i].enabled = false;
            ConfigManager::save();
            publishPump((uint8_t)i);
        }
        return;
    }
}

// ── Publikuj HA Discovery dla jednej pompy ───────────────────
static void _publishDiscovery(int i) {
    char pumpId[32];
    snprintf(pumpId, sizeof(pumpId), "rdp_%d", i);

    String stateTopic = _topic("/pump/") + i + "/state";
    String cmdTopic   = _topic("/pump/") + i + "/cmd";
    String availTopic = _topic("/status");

    // Wspólny blok "device" dla wszystkich encji tej pompy
    // (HA grupuje encje z tym samym identyfikatorem urządzenia)
    char devId[32];
    snprintf(devId, sizeof(devId), "reef_dosepump_%d", i);

    // ── Sensor: ml dzisiaj ───────────────────────────────────
    {
        JsonDocument doc;
        doc["name"]               = String(pumpCal[i].name) + " ml dziś";
        doc["unique_id"]          = String(devId) + "_ml_today";
        doc["state_topic"]        = stateTopic;
        doc["availability_topic"] = availTopic;
        doc["payload_available"]  = "online";
        doc["payload_not_available"] = "offline";
        doc["value_template"]     = "{{ value_json.ml_today | round(2) }}";
        doc["unit_of_measurement"] = "ml";
        doc["icon"]               = "mdi:water";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = String("reef_dosepump_") + i;
        dev["name"]           = pumpCal[i].name;
        dev["model"]          = "ReefDosePump ESP32-S3";
        dev["manufacturer"]   = "DIY";

        String payload;
        serializeJson(doc, payload);
        _mqtt.publish(_disc("sensor", devId, "ml_today").c_str(),
                      payload.c_str(), true);
    }

    // ── Sensor: ml łącznie ───────────────────────────────────
    {
        JsonDocument doc;
        doc["name"]               = String(pumpCal[i].name) + " ml łącznie";
        doc["unique_id"]          = String(devId) + "_ml_total";
        doc["state_topic"]        = stateTopic;
        doc["availability_topic"] = availTopic;
        doc["payload_available"]  = "online";
        doc["payload_not_available"] = "offline";
        doc["value_template"]     = "{{ value_json.ml_total | round(2) }}";
        doc["unit_of_measurement"] = "ml";
        doc["icon"]               = "mdi:counter";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = String("reef_dosepump_") + i;
        dev["name"]           = pumpCal[i].name;

        String payload;
        serializeJson(doc, payload);
        _mqtt.publish(_disc("sensor", devId, "ml_total").c_str(),
                      payload.c_str(), true);
    }

    // ── Switch: włącz/wyłącz pompę ───────────────────────────
    {
        JsonDocument doc;
        doc["name"]               = String(pumpCal[i].name) + " aktywna";
        doc["unique_id"]          = String(devId) + "_enabled";
        doc["state_topic"]        = stateTopic;
        doc["availability_topic"] = availTopic;
        doc["payload_available"]  = "online";
        doc["payload_not_available"] = "offline";
        doc["value_template"]     = "{{ value_json.enabled }}";
        doc["state_on"]           = "true";
        doc["state_off"]          = "false";
        doc["command_topic"]      = cmdTopic;
        doc["payload_on"]         = "{\"action\":\"enable\"}";
        doc["payload_off"]        = "{\"action\":\"disable\"}";
        doc["icon"]               = "mdi:pump";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = String("reef_dosepump_") + i;
        dev["name"]           = pumpCal[i].name;

        String payload;
        serializeJson(doc, payload);
        _mqtt.publish(_disc("switch", devId, "enabled").c_str(),
                      payload.c_str(), true);
    }

    // ── Button: dawkowanie ręczne ─────────────────────────────
    {
        JsonDocument doc;
        doc["name"]               = String(pumpCal[i].name) + " dawkuj";
        doc["unique_id"]          = String(devId) + "_dose_btn";
        doc["availability_topic"] = availTopic;
        doc["payload_available"]  = "online";
        doc["payload_not_available"] = "offline";
        doc["command_topic"]      = cmdTopic;
        doc["payload_press"]      = String("{\"action\":\"dose\",\"ml\":") +
                                    String(pumpCal[i].mlPerDay / max(pumpCal[i].dosesPerDay, 1), 2) +
                                    "}";
        doc["icon"]               = "mdi:water-pump";
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = String("reef_dosepump_") + i;
        dev["name"]           = pumpCal[i].name;

        String payload;
        serializeJson(doc, payload);
        _mqtt.publish(_disc("button", devId, "dose").c_str(),
                      payload.c_str(), true);
    }
}

// ── Połącz z brokerem ────────────────────────────────────────
static bool _connect() {
    if (strlen(netCfg.mqttBroker) == 0) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    _mqtt.setServer(netCfg.mqttBroker, netCfg.mqttPort);
    _mqtt.setCallback(_onMessage);
    _mqtt.setKeepAlive(MQTT_KEEPALIVE);
    _mqtt.setBufferSize(512);
    _mqtt.setSocketTimeout(5);

    String clientId = String(DEVICE_NAME) + "_" + String(ESP.getEfuseMac(), HEX);
    String willTopic = _topic("/status");

    WebServerHandler::wsLogf("MQTT: łączenie z %s:%d ...", netCfg.mqttBroker, netCfg.mqttPort);

    bool ok = (strlen(netCfg.mqttUser) > 0)
              ? _mqtt.connect(clientId.c_str(), netCfg.mqttUser, netCfg.mqttPass,
                              willTopic.c_str(), 1, true, "offline")
              : _mqtt.connect(clientId.c_str(),
                              willTopic.c_str(), 1, true, "offline");

    if (ok) {
        WebServerHandler::wsLogf("MQTT: połączono z %s:%d", netCfg.mqttBroker, netCfg.mqttPort);
        // Opublikuj "online"
        _mqtt.publish(willTopic.c_str(), "online", true);

        // Subskrybuj tematy komend wszystkich pomp
        for (int i = 0; i < NUM_PUMPS; i++) {
            String sub = _topic("/pump/") + i + "/cmd";
            _mqtt.subscribe(sub.c_str(), 1);
        }

        // Publikuj HA Discovery
        for (int i = 0; i < NUM_PUMPS; i++) {
            _publishDiscovery(i);
        }

        // Wstępny publish stanu
        publishAll();
    } else {
        int rc = _mqtt.state();
        WebServerHandler::wsLogf("MQTT: błąd połączenia (rc=%d)", rc);
    }
    return ok;
}

// ============================================================
void begin() {
    if (strlen(netCfg.mqttBroker) == 0) {
        WebServerHandler::wsLogf("MQTT: brak brokera – pomijam");
        return;
    }
    _connect();
}

// ============================================================
void loop() {
    if (strlen(netCfg.mqttBroker) == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;  // nie próbuj bez WiFi

    if (!_mqtt.connected()) {
        static uint32_t lastAttempt = 0;
        if (millis() - lastAttempt > 10000) {
            lastAttempt = millis();
            WebServerHandler::wsLogf("MQTT: próba ponownego połączenia...");
            _connect();
        }
    } else {
        _mqtt.loop();
    }
}

// ============================================================
void publishPump(uint8_t idx) {
    if (!_mqtt.connected() || idx >= NUM_PUMPS) return;

    JsonDocument doc;
    doc["idx"]      = idx;
    doc["name"]     = pumpCal[idx].name;
    doc["enabled"]  = pumpCal[idx].enabled;
    doc["running"]  = PumpController::isRunning(idx);
    doc["ml_today"] = doseRec[idx].mlToday;
    doc["ml_total"] = doseRec[idx].mlTotal;
    doc["ml_day"]   = pumpCal[idx].mlPerDay;

    if (doseRec[idx].lastDose > 0) {
        char buf[32];
        struct tm lt;
        time_t t = doseRec[idx].lastDose;
        localtime_r(&t, &lt);
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &lt);
        doc["last_dose"] = buf;
    }

    String stateTopic = _topic("/pump/") + idx + "/state";
    String payload;
    serializeJson(doc, payload);
    _mqtt.publish(stateTopic.c_str(), payload.c_str(), true);
}

// ============================================================
void publishAll() {
    for (int i = 0; i < NUM_PUMPS; i++) publishPump(i);
}

// ============================================================
bool isConnected() {
    return _mqtt.connected();
}

} // namespace MqttHandler
