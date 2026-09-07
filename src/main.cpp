#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <time.h>
#include <esp_sntp.h>

#include "config.h"
#include "config_manager.h"
#include "pump_controller.h"
#include "web_server_handler.h"
#include "mqtt_handler.h"

// ============================================================
//  GLOBALNE INSTANCJE – deklarowane extern w config.h
// ============================================================
PumpCalibration pumpCal[NUM_PUMPS];
DoseRecord doseRec[NUM_PUMPS];
NetworkConfig netCfg;
GeneralSettings genCfg;

// ============================================================
//  ZMIENNE WEWNĘTRZNE
// ============================================================
static bool _wifiConnected = false;
static bool _ntpSynced = false;
static uint32_t _lastScheduler = 0;
static uint32_t _lastMqttPub = 0;
static int _lastDay = -1;

// Fallback (bez NTP): śledzenie czasu dawkowania przez millis()
static uint32_t _lastDoseMs[NUM_PUMPS] = {};
static uint32_t _globalLastDoseMs = 0;
static uint32_t _fallbackDayStartMs = 0;

// ============================================================
//  PROTOTYPY
// ============================================================
static void setupWiFi();
static void setupNTP();
static void runScheduler();
static void runSchedulerFallback();
static void checkDailyReset();

// ============================================================
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n\n=== %s v%s ===\n", DEVICE_NAME, FIRMWARE_VERSION);

    // 1. Konfiguracja z NVS (LittleFS tylko dla panelu WWW)
    ConfigManager::begin();
    ConfigManager::load();

    // 2. Pompy (PWM + enkodery)
    PumpController::begin();

    // 3. WiFi
    setupWiFi();

    // 4. Web server (zawsze aktywny – też w trybie AP)
    WebServerHandler::begin();

    // 5. MQTT (tylko przy połączeniu STA)
    if (_wifiConnected)
    {
        MqttHandler::begin();
    }

    // 6. NTP
    if (_wifiConnected && genCfg.ntpEnabled)
    {
        setupNTP();
    }

    _fallbackDayStartMs = millis(); // start odliczania doby bez NTP
    WebServerHandler::wsLogf("=== Setup zakończony ===");
}

// ============================================================
void loop()
{
    // ── PumpController – obsługa nieblokujących zadań ────────
    PumpController::loop();

    // ── MQTT keep-alive ──────────────────────────────────────
    MqttHandler::loop();

    // ── WebSocket – czyszczenie nieaktywnych klientów ────────
    WebServerHandler::loop();

    // ── WiFi: wykryj utratę i ponowne połączenie ───────────────
    if (netCfg.configured)
    {
        bool wifiNow = (WiFi.status() == WL_CONNECTED);
        if (_wifiConnected && !wifiNow)
        {
            _wifiConnected = false;
            WebServerHandler::wsLogf("WiFi: utracono połączenie – oczekuję na reconnect...");
        }
        else if (!_wifiConnected && wifiNow)
        {
            _wifiConnected = true;
            WebServerHandler::wsLogf("WiFi: ponownie połączono – IP: %s, RSSI: %d dBm",
                                     WiFi.localIP().toString().c_str(), WiFi.RSSI());
            MDNS.begin(DEVICE_NAME);
            if (!MqttHandler::isConnected())
                MqttHandler::begin();
            if (!_ntpSynced && genCfg.ntpEnabled)
                setupNTP();
        }
    }

    // ── Scheduler dozowania (co 10 s) ────────────────────────
    if (millis() - _lastScheduler >= 10000UL)
    {
        _lastScheduler = millis();
        if (_ntpSynced)
            runScheduler();
        else
            runSchedulerFallback(); // działa bez WiFi/NTP
    }

    // ── Publikuj stan MQTT (co 60 s) ─────────────────────────
    if (_wifiConnected && millis() - _lastMqttPub >= 60000UL)
    {
        _lastMqttPub = millis();
        MqttHandler::publishAll();
    }

    // ── Dzienny reset liczników ml ───────────────────────────
    checkDailyReset();
}

// ============================================================
static void setupWiFi()
{
    if (!netCfg.configured || strlen(netCfg.ssid) == 0)
    {
        WebServerHandler::wsLogf("WiFi: tryb AP – SSID=%s", AP_SSID);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        WiFi.setTxPower(WIFI_POWER_11dBm); // ogranicz moc AP – zmniejsza grzanie
        WebServerHandler::wsLogf("WiFi AP IP: %s", WiFi.softAPIP().toString().c_str());
        _wifiConnected = false;
        return;
    }

    WebServerHandler::wsLogf("WiFi: łączenie z \"%s\"...", netCfg.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(DEVICE_NAME);
    WiFi.setSleep(false); // stabilniejsze połączenie/responsywność niż domyślny modem-sleep

    // Statyczne IP – tylko jeśli kompletne i spójne z bramą (ip & mask == gw & mask).
    // Niespójny/nieaktualny wpis (np. po zmianie sieci na dom_IoT) nie blokuje
    // już asocjacji L2 z martwą konfiguracją L3 – po prostu używamy DHCP.
    bool useStatic = (strlen(netCfg.staticIp) > 6 && strlen(netCfg.gateway) > 6);
    IPAddress ip, gw, sn, dns(8, 8, 8, 8);
    if (useStatic)
    {
        useStatic = ip.fromString(netCfg.staticIp) &&
                    gw.fromString(netCfg.gateway) &&
                    sn.fromString(netCfg.subnet) &&
                    ((uint32_t)ip & (uint32_t)sn) == ((uint32_t)gw & (uint32_t)sn);
        if (!useStatic)
        {
            WebServerHandler::wsLogf("WiFi: statyczne IP %s niespójne z bramą %s/%s – używam DHCP",
                                     netCfg.staticIp, netCfg.gateway, netCfg.subnet);
        }
    }
    if (useStatic)
    {
        WiFi.config(ip, gw, sn, dns); // dns wymagany przez SNTP (unika deadlocku z AsyncTCP)
    }

    // Kilka prób połączenia zanim uznamy sieć za niedostępną – pojedyncza próba
    // bywa zawodna zaraz po starcie AP sąsiedniej sieci / routera.
    constexpr int kMaxAttempts = 3;
    bool connected = false;
    for (int attempt = 1; attempt <= kMaxAttempts && !connected; attempt++)
    {
        if (attempt > 1)
        {
            WebServerHandler::wsLogf("WiFi: próba %d/%d...", attempt, kMaxAttempts);
            WiFi.disconnect();
            delay(200);
        }
        WiFi.begin(netCfg.ssid, netCfg.password);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL)
        {
            delay(500);
            Serial.print('.');
        }
        Serial.println();
        connected = (WiFi.status() == WL_CONNECTED);
    }

    if (connected)
    {
        _wifiConnected = true;
        WebServerHandler::wsLogf("WiFi: OK – IP: %s, RSSI: %d dBm",
                                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
        WiFi.setTxPower(WIFI_POWER_21dBm);
        if (MDNS.begin(DEVICE_NAME))
            WebServerHandler::wsLogf("mDNS: dostępny pod %s.local", DEVICE_NAME);
    }
    else
    {
        WebServerHandler::wsLogf("WiFi: timeout – przełączam na tryb AP");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        WiFi.setTxPower(WIFI_POWER_21dBm);
        _wifiConnected = false;
    }
}

// ============================================================
static void setupNTP()
{
    configTzTime(NTP_TIMEZONE, NTP_SERVER1, NTP_SERVER2);
    Serial.print("NTP: synchronizacja");
    struct tm ti;
    uint32_t start = millis();
    while (!getLocalTime(&ti) && millis() - start < 10000UL)
    {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (getLocalTime(&ti))
    {
        _ntpSynced = true;
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        WebServerHandler::wsLogf("NTP: OK – %s", buf);
    }
    else
    {
        WebServerHandler::wsLogf("NTP: synchronizacja nieudana");
        esp_sntp_stop();
    }
}

// ============================================================
//  Scheduler FALLBACK – działa bez NTP, bez okien czasowych
//  Używa millis() zamiast time_t; zapewnia dozowanie nawet bez WiFi
// ============================================================
static void runSchedulerFallback()
{
    uint32_t now = millis();

    // Globalny offset między pompami
    uint32_t offsetMs = (uint32_t)genCfg.pumpIntervalMin * 60000UL;
    if (_globalLastDoseMs > 0 && (now - _globalLastDoseMs) < offsetMs)
        return;

    for (int i = 0; i < NUM_PUMPS; i++)
    {
        if (!pumpCal[i].enabled)
        {
            WebServerHandler::wsLogf("Scheduler(fallback): pompa %d pominięta (wyłączona)", i);
            continue;
        }
        if (PumpController::isRunning(i))
        {
            WebServerHandler::wsLogf("Scheduler(fallback): pompa %d pominięta (już działa)", i);
            continue;
        }

        int doses = max(pumpCal[i].dosesPerDay, 1);
        float mlPerDose = pumpCal[i].mlPerDay / (float)doses;
        if (mlPerDose <= 0.0f)
        {
            WebServerHandler::wsLogf("Scheduler(fallback): pompa %d pominięta (mlPerDose <= 0)", i);
            continue;
        }

        // Dobowy limit (licznik jest zerowany w checkDailyReset)
        if (doseRec[i].mlToday >= pumpCal[i].mlPerDay * 0.99f)
        {
            WebServerHandler::wsLogf("Scheduler(fallback): pompa %d pominięta (limit dobowy %.2f/%.2f ml)",
                                     i, doseRec[i].mlToday, pumpCal[i].mlPerDay);
            continue;
        }

        // Interwał między dawkami tej pompy
        uint32_t intervalMs = (uint32_t)(86400000UL / (uint32_t)doses);
        if (_lastDoseMs[i] > 0 && (now - _lastDoseMs[i]) < intervalMs)
        {
            WebServerHandler::wsLogf("Scheduler(fallback): pompa %d pominięta (interwał %lu ms, zostało %lu ms)",
                                     i, (unsigned long)intervalMs,
                                     (unsigned long)(intervalMs - (now - _lastDoseMs[i])));
            continue;
        }

        WebServerHandler::wsLogf("Scheduler(fallback): pompa %d -> %.2f ml (brak NTP, brak okna)",
                                 i, mlPerDose);
        PumpController::dose((uint8_t)i, mlPerDose);
        _lastDoseMs[i] = now;
        _globalLastDoseMs = now;
        break;
    }
}

// ============================================================
static void runScheduler()
{
    struct tm ti;
    if (!getLocalTime(&ti))
        return;
    time_t now = mktime(&ti);
    int nowH = ti.tm_hour;

    // ── Globalny offset – znajdź ostatnią dawkę JAKIEJKOLWIEK pompy ──
    time_t globalLastDose = 0;
    for (int j = 0; j < NUM_PUMPS; j++)
    {
        if (doseRec[j].lastDose > globalLastDose)
            globalLastDose = doseRec[j].lastDose;
    }
    uint32_t offsetSec = (uint32_t)genCfg.pumpIntervalMin * 60UL;
    if (globalLastDose > 0 && (uint32_t)(now - globalLastDose) < offsetSec)
        return;

    for (int i = 0; i < NUM_PUMPS; i++)
    {
        if (!pumpCal[i].enabled)
        {
            WebServerHandler::wsLogf("Scheduler: pompa %d pominięta (wyłączona)", i);
            continue;
        }
        if (PumpController::isRunning(i))
        {
            WebServerHandler::wsLogf("Scheduler: pompa %d pominięta (już działa)", i);
            continue;
        }

        int doses = max(pumpCal[i].dosesPerDay, 1);
        float mlPerDose = pumpCal[i].mlPerDay / (float)doses;
        if (mlPerDose <= 0.0f)
        {
            WebServerHandler::wsLogf("Scheduler: pompa %d pominięta (mlPerDose <= 0)", i);
            continue;
        }

        // Dawka dzienna nie przekroczona
        if (doseRec[i].mlToday >= pumpCal[i].mlPerDay * 0.99f)
        {
            WebServerHandler::wsLogf("Scheduler: pompa %d pominięta (limit dobowy %.2f/%.2f ml)",
                                     i, doseRec[i].mlToday, pumpCal[i].mlPerDay);
            continue;
        }

        // Okno czasowe (obsługa okna nocnego gdy start > end)
        uint8_t ws = pumpCal[i].windowStart;
        uint8_t we = pumpCal[i].windowEnd;
        bool inWindow = (ws < we) ? (nowH >= ws && nowH < we)  // dzienne
                                  : (nowH >= ws || nowH < we); // nocne
        if (!inWindow)
        {
            WebServerHandler::wsLogf("Scheduler: pompa %d pominięta (poza oknem %02d-%02d, teraz %02d:00)",
                                     i, ws, we, nowH);
            continue;
        }

        uint32_t windowSec = 0;
        if (ws == we)
        {
            windowSec = 86400UL;
        }
        else if (ws < we)
        {
            windowSec = (uint32_t)(we - ws) * 3600UL;
        }
        else
        {
            windowSec = (uint32_t)((24 - ws + we) * 3600UL);
        }
        if (windowSec == 0)
            continue;

        // Interwał w obrębie samego okna, a nie całej doby – dzięki temu 5 dawek
        // rozkłada się równomiernie w oknie 07:00–22:00 i nie znika ostatnia porcja.
        uint32_t intervalSec = windowSec / (uint32_t)doses;
        if (intervalSec == 0)
            intervalSec = 60UL;

        if (doseRec[i].lastDose > 0 &&
            (uint32_t)(now - doseRec[i].lastDose) < intervalSec)
        {
            WebServerHandler::wsLogf("Scheduler: pompa %d pominięta (interwał %lu s, zostało %lu s)",
                                     i, (unsigned long)intervalSec,
                                     (unsigned long)(intervalSec - (uint32_t)(now - doseRec[i].lastDose)));
            continue;
        }

        // Dawkuj – tylko jedna pompa na cykl (następna po upływie offsetSec)
        WebServerHandler::wsLogf("Scheduler: pompa %d -> %.2f ml (okno %02d-%02d, interwał=%lu s)",
                                 i, mlPerDose, ws, we, (unsigned long)intervalSec);
        PumpController::dose((uint8_t)i, mlPerDose);
        break;
    }
}

// ============================================================
static void checkDailyReset()
{
    if (!_ntpSynced)
    {
        // Fallback: millis-based reset co 24 h (brak NTP)
        if (millis() - _fallbackDayStartMs >= 86400000UL)
        {
            for (int i = 0; i < NUM_PUMPS; i++)
                doseRec[i].mlToday = 0.0f;
            ConfigManager::saveDoseRecords();
            WebServerHandler::wsLogf("Nowy dzień (fallback): reset liczników ml");
            _fallbackDayStartMs = millis();
        }
        return;
    }

    struct tm ti;
    if (!getLocalTime(&ti))
        return;

    if (ti.tm_mday == _lastDay)
        return;

    if (_lastDay != -1)
    {
        // Nowy dzień – zeruj liczniki
        for (int i = 0; i < NUM_PUMPS; i++)
            doseRec[i].mlToday = 0.0f;
        ConfigManager::saveDoseRecords();
        MqttHandler::publishAll();
        WebServerHandler::wsLogf("Nowy dzień: reset liczników ml");
    }
    _lastDay = ti.tm_mday;
}
