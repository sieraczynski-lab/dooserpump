#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ============================================================
//  WERSJA FIRMWARE
// ============================================================
#define FIRMWARE_VERSION  "1.0.0"
#define DEVICE_NAME       "ReefDosePump"

// ============================================================
//  LICZBA POMP
// ============================================================
#define NUM_PUMPS  4

// ============================================================
//  PINY ESP32-S3 ZERO  (zmieniaj TYLKO tutaj)
// ============================================================
// PWM – silniki bezszczotkowe (BLDC)
constexpr uint8_t PUMP_PWM_PIN[NUM_PUMPS] = { 4, 5, 6, 7 };

// Enkodery silników (kanał A każdego enkodera)
// PCNT unit 0-3 odpowiednio
constexpr uint8_t PUMP_ENC_A_PIN[NUM_PUMPS] = { 8,  9,  10, 11 };
// constexpr uint8_t PUMP_ENC_B_PIN[NUM_PUMPS] = { 12, 13, 14, 15 };

// Kanały PWM LEDC
constexpr uint8_t PUMP_PWM_CHANNEL[NUM_PUMPS] = { 0, 1, 2, 3 };
constexpr uint32_t PUMP_PWM_FREQ   = 20000;   // Hz
constexpr uint8_t  PUMP_PWM_RES    = 10;       // bity (0-1023)

// ============================================================
//  DOMYŚLNE DANE AP (pierwsze uruchomienie)
// ============================================================
#define AP_SSID     "ReefDosePump"
#define AP_PASSWORD "reef1234"
#define AP_IP       "192.168.4.1"

// ============================================================
//  NTP
// ============================================================
#define NTP_SERVER1   "pool.ntp.org"
#define NTP_SERVER2   "time.google.com"
#define NTP_TIMEZONE  "CET-1CEST,M3.5.0,M10.5.0/3"  // Polska

// ============================================================
//  MQTT – wartości domyślne (nadpisywane przez zapisaną konfigurację)
// ============================================================
#define MQTT_PORT_DEFAULT   1883
#define MQTT_KEEPALIVE      60
#define MQTT_BASE_TOPIC     "reef/dosepump"
#define MQTT_HA_PREFIX      "homeassistant"

// ============================================================
//  KONFIGURACJA – NVS (Preferences, namespace "cfg"), NIE LittleFS.
//  LittleFS służy tylko do serwowania panelu WWW (data/index.html) i jest
//  całkowicie kasowany przez `pio run -t uploadfs` – ustawienia/kalibracja/
//  rekordy dawkowania w NVS przeżywają to bez zmian (osobna partycja).
// ============================================================

// ============================================================
//  STAŁE GLOBALNE DOZOWANIA (nadpisywane przez ustawienia WWW)
// ============================================================
#define DEFAULT_PUMP_INTERVAL_MIN   15   // min między kolejnymi pompami
#define DEFAULT_DAILY_DOSES         4    // liczba dawek na dobę
#define DEFAULT_ML_PER_DAY          5.0f // ml/dobę dla każdej pompy

// Kalibracja: impulsy enkodera na ml
#define DEFAULT_PULSES_PER_ML       200.0f

// ============================================================
//  STRUKTURY DANYCH
// ============================================================

struct PumpCalibration {
    float   pulsesPerMl   = DEFAULT_PULSES_PER_ML;
    float   mlPerDay      = DEFAULT_ML_PER_DAY;
    int     dosesPerDay   = DEFAULT_DAILY_DOSES;
    char    name[20]      = "Pompa";
    bool    enabled       = true;
    uint8_t windowStart   = 0;   // godz. początku okna dawkowania (0..23)
    uint8_t windowEnd     = 24;  // godz. końca okna dawkowania (1..24; 24 = koniec doby)
};

struct DoseRecord {
    float  mlToday        = 0.0f;
    float  mlTotal        = 0.0f;   // od ostatniego resetu
    long   pulsesTotal    = 0;
    time_t lastDose       = 0;
};

struct NetworkConfig {
    // Celowo BEZ żadnych domyślnych danych domowej sieci wpisanych na sztywno
    // w kodzie – to był relikt z wczesnego developmentu (realne SSID/hasło
    // w źródłach, do tego re-aplikowane po każdym factory reset/świeżym flashu).
    // Docelowy przepływ: świeże urządzenie startuje w AP (patrz setupWiFi()
    // w main.cpp – pusty ssid => tryb AP) i sieć domową wpisuje się raz,
    // przez panel WWW; stąd trafia do NVS (ConfigManager) i tam zostaje.
    char   ssid[64]       = "";
    char   password[64]   = "";
    // Puste IP/gateway = DHCP (zalecane – patrz help.md). Statyczne IP ma sens
    // tylko dla konkretnej sieci/routera; po zmianie sieci stary wpis
    // wskazywałby na martwą bramę i STA nie połączyłoby się mimo poprawnego
    // SSID/hasła – dlatego setupWiFi() dodatkowo waliduje ip/maskę/bramę.
    char   staticIp[16]   = "";
    char   gateway[16]    = "";
    char   subnet[16]     = "255.255.255.0";
    char   mqttBroker[64] = "";
    uint16_t mqttPort     = MQTT_PORT_DEFAULT;
    char   mqttUser[32]   = "";
    char   mqttPass[32]   = "";
    char   mqttTopic[64]  = MQTT_BASE_TOPIC;
    char   webUser[32]    = "admin";
    char   webPass[32]    = "admin";
    bool   configured     = false;   // true = spróbuj STA z powyższym ssid/password, false = tryb AP z AP_SSID/AP_PASSWORD
};

struct GeneralSettings {
    uint16_t pumpIntervalMin  = DEFAULT_PUMP_INTERVAL_MIN;
    uint8_t  dosesPerDay      = DEFAULT_DAILY_DOSES;
    bool     ntpEnabled       = true;
};

// ============================================================
//  GLOBALNY STAN (definiowany w main.cpp, extern wszędzie)
// ============================================================
extern PumpCalibration  pumpCal[NUM_PUMPS];
extern DoseRecord       doseRec[NUM_PUMPS];
extern NetworkConfig    netCfg;
extern GeneralSettings  genCfg;
