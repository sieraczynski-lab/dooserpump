#pragma once
#include "config.h"

namespace PumpController {

    // Inicjalizuje LEDC PWM i przerwania enkoderów
    void begin();

    // Wywołuj z loop() – obsługuje nieblokujące zadania dozowania
    void loop();

    // Rozpoczyna nieblokujące dozowanie ml na pompie idx
    void dose(uint8_t idx, float ml);

    // Zatrzymuje pompę idx
    void stop(uint8_t idx);

    // Zatrzymuje wszystkie pompy
    void stopAll();

    // true gdy pompa aktualnie dozuje
    bool isRunning(uint8_t idx);

    // Aktualny licznik impulsów enkodera (od ostatniego resetPulses)
    long getPulses(uint8_t idx);

    // Resetuje licznik impulsów (przydatne przy kalibracji)
    void resetPulses(uint8_t idx);

    // Ręczne ustawienie duty PWM (0-1023) – do testów/kalibracji
    void setPWM(uint8_t idx, uint16_t duty);

} // namespace PumpController
