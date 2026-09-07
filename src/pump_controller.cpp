#include "pump_controller.h"
#include "web_server_handler.h"
#include "config_manager.h"
#include <Arduino.h>

namespace PumpController {

// ── Duty domyślne (zakres 0–1023 przy 10-bit) ──────────────
static constexpr uint16_t PWM_MAX      = (1 << PUMP_PWM_RES) - 1;  // 1023
static constexpr uint16_t DEFAULT_DUTY = 700;   // ~68 % siły
// Sterownik aktywny LOW: HIGH = stop, LOW = obroty
// Duty wysyłane do LEDC = PWM_MAX - żądane_duty

// ── Volatile liczniki impulsów enkodera ─────────────────────
volatile long _enc[NUM_PUMPS] = {0, 0, 0, 0};

void IRAM_ATTR _isr0() { _enc[0]++; }
void IRAM_ATTR _isr1() { _enc[1]++; }
void IRAM_ATTR _isr2() { _enc[2]++; }
void IRAM_ATTR _isr3() { _enc[3]++; }

typedef void (*IsrFn)();
static const IsrFn _isrs[NUM_PUMPS] = { _isr0, _isr1, _isr2, _isr3 };

// ── Struktura zadania dozowania ──────────────────────────────
struct DoseJob {
    bool     active        = false;
    float    mlRequested   = 0.0f;
    long     targetPulses  = 0;
    long     startCount    = 0;
    uint32_t startMs       = 0;
    uint32_t timeoutMs     = 30000;
};
static DoseJob _jobs[NUM_PUMPS];

// ============================================================
void begin() {
    for (int i = 0; i < NUM_PUMPS; i++) {
        // Wymuś LOW zanim LEDC przejmie pin – zapobiega kręceniu pompy podczas bootu
        pinMode(PUMP_PWM_PIN[i], OUTPUT);
        digitalWrite(PUMP_PWM_PIN[i], HIGH);

        // ESP32 Arduino 3.x – API oparte na pinie (nie kanale)
        ledcAttach(PUMP_PWM_PIN[i], PUMP_PWM_FREQ, PUMP_PWM_RES);
        ledcWrite(PUMP_PWM_PIN[i], PWM_MAX);  // pompa zatrzymana (active-LOW: HIGH = stop)

        // Enkoder – tylko tryb pinu; przerwanie dołączamy dopiero
        // gdy pompa jest aktywna (unikamy false-triggers na luźnych pinach)
        pinMode(PUMP_ENC_A_PIN[i], INPUT_PULLUP);
    }
    Serial.println("PumpController: inicjalizacja OK");
}

// ── Powód zatrzymania pompy – wpływa tylko na treść logu ─────
enum class StopReason { Completed, Timeout, Manual, Overridden };

// ── Zatrzymuje pompę idx i – jeśli coś faktycznie dozowała –
//    dolicza rzeczywiście dostarczoną objętość do doseRec[idx].
//    Wywoływana z KAŻDEGO miejsca, które może przerwać dozowanie
//    (timeout, ręczny stop, nowa dawka nadpisująca poprzednią),
//    żeby dobowy limit (mlToday) nigdy nie pomijał częściowo
//    wykonanej dawki.
static void _stopAndAccount(uint8_t idx, StopReason reason) {
    bool wasActive = _jobs[idx].active;

    detachInterrupt(digitalPinToInterrupt(PUMP_ENC_A_PIN[idx]));
    ledcWrite(PUMP_PWM_PIN[idx], PWM_MAX);  // active-LOW: HIGH = stop
    _jobs[idx].active = false;

    if (!wasActive) return; // nic nie było w trakcie – nie ma czego rozliczać

    long pulsesDone = _enc[idx] - _jobs[idx].startCount;
    float mlActual  = (pulsesDone > 0 && pumpCal[idx].pulsesPerMl > 0.0f)
                      ? (float)pulsesDone / pumpCal[idx].pulsesPerMl
                      : 0.0f;

    switch (reason) {
        case StopReason::Completed:
            WebServerHandler::wsLogf("Pompa %d: dawka OK – %.2f ml (%ld impulsów)",
                          idx, mlActual, pulsesDone);
            break;
        case StopReason::Timeout:
            WebServerHandler::wsLogf("Pompa %d: TIMEOUT (%.2f ml / %.2f ml docelowych)",
                          idx, mlActual, _jobs[idx].mlRequested);
            break;
        case StopReason::Manual:
            WebServerHandler::wsLogf("Pompa %d: zatrzymana ręcznie – %.2f ml z %.2f ml docelowych",
                          idx, mlActual, _jobs[idx].mlRequested);
            break;
        case StopReason::Overridden:
            WebServerHandler::wsLogf("Pompa %d: przerwana nową dawką – doliczono %.2f ml z poprzedniej",
                          idx, mlActual);
            break;
    }

    if (pulsesDone <= 0) return; // nic realnie nie popłynęło – nie zaśmiecaj rekordów

    doseRec[idx].mlToday     += mlActual;
    doseRec[idx].mlTotal     += mlActual;
    doseRec[idx].pulsesTotal += pulsesDone;

    struct tm ti;
    if (getLocalTime(&ti) && ti.tm_year >= 124) {   // rok >= 2024 → czas zsynchronizowany
        doseRec[idx].lastDose = mktime(&ti);
    }

    ConfigManager::saveDoseRecords();
}

// ============================================================
void loop() {
    for (int i = 0; i < NUM_PUMPS; i++) {
        if (!_jobs[i].active) continue;

        long pulsesDone = _enc[i] - _jobs[i].startCount;
        bool done      = (pulsesDone >= _jobs[i].targetPulses);
        bool timedOut  = ((millis() - _jobs[i].startMs) > _jobs[i].timeoutMs);

        if (!done && !timedOut) continue;

        _stopAndAccount(i, (timedOut && !done) ? StopReason::Timeout : StopReason::Completed);
    }
}

// ============================================================
void dose(uint8_t idx, float ml) {
    if (idx >= NUM_PUMPS) return;
    if (ml <= 0.0f) return;

    if (_jobs[idx].active) _stopAndAccount(idx, StopReason::Overridden);

    long targetPulses = (pumpCal[idx].pulsesPerMl > 0.0f)
                        ? (long)(ml * pumpCal[idx].pulsesPerMl)
                        : 1000L;  // fallback jeśli brak kalibracji

    // Timeout: min 10 s, max 5 min; szacuj 2 ml/s przy nominalnym przepływie
    uint32_t estMs  = (uint32_t)((ml / 2.0f) * 1000.0f);
    uint32_t toMs   = max(10000UL, estMs * 3UL);  // 3x margines
    toMs = min(toMs, 300000UL);                   // max 5 min

    _jobs[idx].active       = true;
    _jobs[idx].mlRequested  = ml;
    _jobs[idx].targetPulses = targetPulses;
    _jobs[idx].startCount   = _enc[idx];
    _jobs[idx].startMs      = millis();
    _jobs[idx].timeoutMs    = toMs;

    // Dołącz przerwanie dokładnie przed uruchomieniem pompy
    attachInterrupt(digitalPinToInterrupt(PUMP_ENC_A_PIN[idx]), _isrs[idx], RISING);

    ledcWrite(PUMP_PWM_PIN[idx], PWM_MAX - DEFAULT_DUTY);  // active-LOW: inwersja duty
    WebServerHandler::wsLogf("Pompa %d: start %.2f ml (%ld imp, timeout %lu ms)",
                  idx, ml, targetPulses, toMs);
}

// ============================================================
void stop(uint8_t idx) {
    if (idx >= NUM_PUMPS) return;
    _stopAndAccount(idx, StopReason::Manual);
}

void stopAll() {
    for (int i = 0; i < NUM_PUMPS; i++) stop(i);
}

bool isRunning(uint8_t idx) {
    return (idx < NUM_PUMPS) && _jobs[idx].active;
}

long getPulses(uint8_t idx) {
    if (idx >= NUM_PUMPS) return 0L;
    noInterrupts();
    long v = _enc[idx];
    interrupts();
    return v;
}

void resetPulses(uint8_t idx) {
    if (idx >= NUM_PUMPS) return;
    noInterrupts();
    _enc[idx] = 0;
    interrupts();
}

void setPWM(uint8_t idx, uint16_t duty) {
    if (idx >= NUM_PUMPS) return;
    duty = (duty > PWM_MAX) ? PWM_MAX : duty;

    if (duty == 0) {
        // Zatrzymaj – odepnij przerwanie, postaw HIGH (active-LOW)
        detachInterrupt(digitalPinToInterrupt(PUMP_ENC_A_PIN[idx]));
        ledcWrite(PUMP_PWM_PIN[idx], PWM_MAX);
    } else {
        // Uruchom – podepnij przerwanie, wyślij zinwertowane duty
        attachInterrupt(digitalPinToInterrupt(PUMP_ENC_A_PIN[idx]),
                        _isrs[idx], RISING);
        ledcWrite(PUMP_PWM_PIN[idx], PWM_MAX - duty);
    }
}

} // namespace PumpController
