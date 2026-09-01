#include "web_server_handler.h"
#include "config.h"
#include "config_manager.h"
#include "pump_controller.h"
#include "mqtt_handler.h"

#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Update.h>
#include <stdarg.h>

namespace WebServerHandler {

static AsyncWebServer _server(80);
static AsyncWebSocket  _ws("/ws/log");

// ── Ring-buffer ostatnich 60 linii (128 znaków każda) ────────
static constexpr int LOG_LINES = 60;
static constexpr int LOG_LINE  = 128;
static char _logBuf[LOG_LINES][LOG_LINE];
static int  _logHead  = 0;
static int  _logCount = 0;

static void _bufAppend(const char* msg) {
    strncpy(_logBuf[_logHead], msg, LOG_LINE - 1);
    _logBuf[_logHead][LOG_LINE - 1] = '\0';
    _logHead = (_logHead + 1) % LOG_LINES;
    if (_logCount < LOG_LINES) _logCount++;
}

// ── Publiczne API ────────────────────────────────────────────
void wsLog(const char* msg) {
    _bufAppend(msg);
    if (_ws.count() > 0 && WiFi.status() == WL_CONNECTED) _ws.textAll(msg);
}

void wsLogf(const char* fmt, ...) {
    char buf[LOG_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    int len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    Serial.println(buf);
    _bufAppend(buf);
    if (_ws.count() > 0 && WiFi.status() == WL_CONNECTED) _ws.textAll(buf);
}

void loop() {
    _ws.cleanupClients();
}

// ── Pomocnik: sprawdź Basic Auth ─────────────────────────────
static bool _auth(AsyncWebServerRequest* req) {
    if (!req->authenticate(netCfg.webUser, netCfg.webPass)) {
        req->requestAuthentication("ReefDosePump");
        return false;
    }
    return true;
}

// ── Pomocnik: JSON error ─────────────────────────────────────
static void _sendError(AsyncWebServerRequest* req, int code, const char* msg) {
    String body = String("{\"error\":\"") + msg + "\"}";
    req->send(code, "application/json", body);
}

// ── Pomocnik: JSON ok ────────────────────────────────────────
static void _sendOk(AsyncWebServerRequest* req) {
    req->send(200, "application/json", "{\"ok\":true}");
}

// ── GET /api/status ──────────────────────────────────────────
static void _handleStatus(AsyncWebServerRequest* req) {
    if (!_auth(req)) return;

    JsonDocument doc;
    doc["fw"]      = FIRMWARE_VERSION;
    doc["device"]  = DEVICE_NAME;
    doc["uptime"]  = millis() / 1000;
    doc["heap"]    = esp_get_free_heap_size();

    struct tm ti;
    if (getLocalTime(&ti)) {
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &ti);
        doc["time"] = buf;
    }

    doc["wifi_ssid"] = WiFi.SSID();
    doc["wifi_ip"]   = WiFi.localIP().toString();
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["mqtt_connected"] = MqttHandler::isConnected();

    JsonArray pumps = doc["pumps"].to<JsonArray>();
    for (int i = 0; i < NUM_PUMPS; i++) {
        JsonObject p = pumps.add<JsonObject>();
        p["idx"]      = i;
        p["name"]     = pumpCal[i].name;
        p["enabled"]  = pumpCal[i].enabled;
        p["running"]  = PumpController::isRunning(i);
        p["ml_today"] = doseRec[i].mlToday;
        p["ml_total"] = doseRec[i].mlTotal;
        p["ml_day"]   = pumpCal[i].mlPerDay;
        p["doses"]    = pumpCal[i].dosesPerDay;
        if (doseRec[i].lastDose > 0) {
            char buf[32];
            struct tm lt;
            time_t t = doseRec[i].lastDose;
            localtime_r(&t, &lt);
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &lt);
            p["last_dose"] = buf;
        } else {
            p["last_dose"] = nullptr;
        }
    }

    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    serializeJson(doc, *resp);
    req->send(resp);
}

// ── GET /api/config ──────────────────────────────────────────
static void _handleGetConfig(AsyncWebServerRequest* req) {
    if (!_auth(req)) return;
    req->send(200, "application/json", ConfigManager::getConfigJson());
}

// ── POST /api/config ─────────────────────────────────────────
static void _handlePostConfig(AsyncWebServerRequest* req, JsonVariant& body) {
    if (!_auth(req)) return;

    String json;
    serializeJson(body, json);
    if (ConfigManager::applyConfigJson(json)) {
        _sendOk(req);
    } else {
        _sendError(req, 400, "Nieprawidłowy JSON");
    }
}

// ── POST /api/pump/{id}/dose  body: {"ml":2.5} ───────────────
static void _handleDose(AsyncWebServerRequest* req, JsonVariant& body) {
    if (!_auth(req)) return;

    int idx = req->pathArg(0).toInt();
    if (idx < 0 || idx >= NUM_PUMPS) {
        _sendError(req, 404, "Nieznana pompa");
        return;
    }
    if (!pumpCal[idx].enabled) {
        _sendError(req, 409, "Pompa wyłączona");
        return;
    }

    float ml = body["ml"] | 0.0f;
    if (ml <= 0.0f || ml > 50.0f) {
        _sendError(req, 400, "Nieprawidłowa ilość ml (0 < ml ≤ 50)");
        return;
    }

    PumpController::dose((uint8_t)idx, ml);
    _sendOk(req);
}

// ── POST /api/pump/{id}/stop ──────────────────────────────────
static void _handleStop(AsyncWebServerRequest* req) {
    if (!_auth(req)) return;

    int idx = req->pathArg(0).toInt();
    if (idx < 0 || idx >= NUM_PUMPS) {
        _sendError(req, 404, "Nieznana pompa");
        return;
    }
    PumpController::stop((uint8_t)idx);
    _sendOk(req);
}

// ── POST /api/calibrate/{id}/start ───────────────────────────
// Uruchamia pompę idx z pełnym PWM i resetuje licznik impulsów
static void _handleCalStart(AsyncWebServerRequest* req) {
    if (!_auth(req)) return;

    int idx = req->pathArg(0).toInt();
    if (idx < 0 || idx >= NUM_PUMPS) {
        _sendError(req, 404, "Nieznana pompa");
        return;
    }
    PumpController::resetPulses((uint8_t)idx);
    PumpController::setPWM((uint8_t)idx, 700);  // domyślne duty
    Serial.printf("Kalibracja pompa %d: start\n", idx);
    _sendOk(req);
}

// ── POST /api/calibrate/{id}/finish  body: {"ml":X} ──────────
// Zatrzymuje pompę, oblicza pulsesPerMl i zapisuje
static void _handleCalFinish(AsyncWebServerRequest* req, JsonVariant& body) {
    if (!_auth(req)) return;

    int idx = req->pathArg(0).toInt();
    if (idx < 0 || idx >= NUM_PUMPS) {
        _sendError(req, 404, "Nieznana pompa");
        return;
    }

    float ml = body["ml"] | 0.0f;
    if (ml <= 0.0f) {
        _sendError(req, 400, "Brak pola ml > 0");
        return;
    }

    PumpController::stop((uint8_t)idx);
    long pulses = PumpController::getPulses((uint8_t)idx);

    if (pulses <= 0) {
        _sendError(req, 409, "Brak impulsów – sprawdź enkoder");
        return;
    }

    pumpCal[idx].pulsesPerMl = (float)pulses / ml;
    ConfigManager::save();

    JsonDocument doc;
    doc["ok"]      = true;
    doc["pulses"]  = pulses;
    doc["ml"]      = ml;
    doc["ppm"]     = pumpCal[idx].pulsesPerMl;

    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    serializeJson(doc, *resp);
    req->send(resp);

    Serial.printf("Kalibracja pompa %d: %ld imp / %.2f ml = %.2f ppm\n",
                  idx, pulses, ml, pumpCal[idx].pulsesPerMl);
}

// ── POST /api/reset ────────────────────────────────────────────
static void _handleReset(AsyncWebServerRequest* req) {
    if (!_auth(req)) return;
    _sendOk(req);
    delay(200);
    ConfigManager::resetToDefaults();
    ESP.restart();
}

// ── POST /api/restart ──────────────────────────────────────────
static void _handleRestart(AsyncWebServerRequest* req) {
    if (!_auth(req)) return;
    _sendOk(req);
    delay(200);
    ESP.restart();
}

// ── POST /update – wgrywanie firmware lub filesystem OTA przez HTTP ──
// Firmware:    curl -u admin:admin -F "firmware=@firmware.bin"  http://IP/update
// Filesystem:  curl -u admin:admin -F "firmware=@littlefs.bin"  http://IP/update
static void _handleOtaUpload(AsyncWebServerRequest* req,
                              const String& filename,
                              size_t index, uint8_t* data,
                              size_t len, bool final) {
    if (!req->authenticate(netCfg.webUser, netCfg.webPass)) {
        return req->requestAuthentication("ReefDosePump");
    }
    if (!index) {
        // Wykryj typ po nazwie pliku: littlefs / spiffs → filesystem, reszta → firmware
        bool isFS = filename.indexOf("littlefs") >= 0 ||
                    filename.indexOf("spiffs")   >= 0;
        int updateType = isFS ? U_SPIFFS : U_FLASH;
        WebServerHandler::wsLogf("OTA: start '%s' (%s)", filename.c_str(),
                                 isFS ? "filesystem" : "firmware");
        PumpController::stopAll();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateType)) {
            Update.printError(Serial);
        }
    }
    if (Update.write(data, len) != len) {
        Update.printError(Serial);
    }
    if (final) {
        if (Update.end(true)) {
            WebServerHandler::wsLogf("OTA: OK (%u B) – restart", index + len);
        } else {
            Update.printError(Serial);
        }
    }
}

// ── GET /api/pulses/{id} – aktualny licznik impulsów ──────────
static void _handlePulses(AsyncWebServerRequest* req) {
    if (!_auth(req)) return;

    int idx = req->pathArg(0).toInt();
    if (idx < 0 || idx >= NUM_PUMPS) {
        _sendError(req, 404, "Nieznana pompa");
        return;
    }

    JsonDocument doc;
    doc["idx"]    = idx;
    doc["pulses"] = PumpController::getPulses((uint8_t)idx);

    AsyncResponseStream* resp = req->beginResponseStream("application/json");
    serializeJson(doc, *resp);
    req->send(resp);
}

// ============================================================
void begin() {
    // ── Strona główna – auth przez ten sam _auth() co API ────
    // serveStatic().setAuthentication() używa innego realm niż
    // req->requestAuthentication("ReefDosePump"), co powoduje
    // pętlę dialogów hasła w przeglądarce. Zamiast tego
    // przechwytujemy "/" ręcznie z tym samym _auth().
    _server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!_auth(req)) return;
        req->send(LittleFS, "/index.html", "text/html");
    });

    // ── Pozostałe pliki statyczne z LittleFS (bez auth) ─────
    _server.serveStatic("/", LittleFS, "/")
           .setDefaultFile("index.html");

    // ── REST API ──────────────────────────────────────────────
    _server.on("/api/status", HTTP_GET, _handleStatus);
    _server.on("/api/config", HTTP_GET, _handleGetConfig);
    _server.on("/api/restart", HTTP_POST, _handleRestart);
    _server.on("/api/reset",   HTTP_POST, _handleReset);

    // HTTP OTA – zastępuje ArduinoOTA (brak konfliktu z AsyncTCP)
    _server.on("/update", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!req->authenticate(netCfg.webUser, netCfg.webPass)) {
                return req->requestAuthentication("ReefDosePump");
            }
            bool ok = !Update.hasError();
            AsyncWebServerResponse* resp = req->beginResponse(
                200, "text/plain", ok ? "OK – restartuję" : "BŁĄD OTA");
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) { delay(500); ESP.restart(); }
        },
        _handleOtaUpload);

    // Endpointy z JSON body (AsyncCallbackJsonWebHandler + regex nie przekazuje pathArgs
    // w ESPAsyncWebServer v3.x – używamy _server.on() z ręcznym akumulowaniem body)

    _server.addHandler(new AsyncCallbackJsonWebHandler(
        "/api/config", [](AsyncWebServerRequest* req, JsonVariant& body) {
            _handlePostConfig(req, body);
        }));

    _server.on("^\\/api\\/pump\\/([0-9])\\/dose$", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!req->_tempObject) { req->send(400, "application/json", "{\"error\":\"No body\"}"); return; }
            JsonDocument doc;
            DeserializationError e = deserializeJson(doc, (const char*)req->_tempObject);
            free(req->_tempObject); req->_tempObject = nullptr;
            if (e) { req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }
            JsonVariant v = doc.as<JsonVariant>(); _handleDose(req, v);
        },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) req->_tempObject = malloc(total + 1);
            if (req->_tempObject) {
                memcpy((uint8_t*)req->_tempObject + index, data, len);
                if (index + len >= total) ((char*)req->_tempObject)[total] = '\0';
            }
        });

    _server.on("^\\/api\\/calibrate\\/([0-9])\\/finish$", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!req->_tempObject) { req->send(400, "application/json", "{\"error\":\"No body\"}"); return; }
            JsonDocument doc;
            DeserializationError e = deserializeJson(doc, (const char*)req->_tempObject);
            free(req->_tempObject); req->_tempObject = nullptr;
            if (e) { req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }
            JsonVariant v = doc.as<JsonVariant>(); _handleCalFinish(req, v);
        },
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) req->_tempObject = malloc(total + 1);
            if (req->_tempObject) {
                memcpy((uint8_t*)req->_tempObject + index, data, len);
                if (index + len >= total) ((char*)req->_tempObject)[total] = '\0';
            }
        });

    // Endpointy bez body
    _server.on("^\\/api\\/pump\\/([0-9])\\/stop$", HTTP_POST,
        [](AsyncWebServerRequest* req) { _handleStop(req); });

    _server.on("^\\/api\\/calibrate\\/([0-9])\\/start$", HTTP_POST,
        [](AsyncWebServerRequest* req) { _handleCalStart(req); });

    _server.on("^\\/api\\/pulses\\/([0-9])$", HTTP_GET,
        [](AsyncWebServerRequest* req) { _handlePulses(req); });

    // ── Fallback 404 ─────────────────────────────────────────
    _server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "application/json", "{\"error\":\"Not found\"}");
    });

    // ── WebSocket /ws/log ─────────────────────────────────────
    _ws.onEvent([](AsyncWebSocket* /*srv*/, AsyncWebSocketClient* client,
                   AwsEventType type, void* /*arg*/,
                   uint8_t* /*data*/, size_t /*len*/) {
        if (type == WS_EVT_CONNECT) {
            // Wyślij ring-buffer do nowego klienta
            int start = (_logCount < LOG_LINES) ? 0 : _logHead;
            for (int i = 0; i < _logCount; i++) {
                int idx = (start + i) % LOG_LINES;
                client->text(_logBuf[idx]);
            }
        }
    });
    _server.addHandler(&_ws);

    _server.begin();
    Serial.println("WebServer: nasłuchuje na porcie 80");
}

} // namespace WebServerHandler
