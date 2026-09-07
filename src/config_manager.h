#pragma once
#include "config.h"

namespace ConfigManager {

    // Montuje LittleFS (panel WWW) – ustawienia same w sobie żyją w NVS
    void begin();

    // Wczytuje konfigurację z NVS → zwraca true przy sukcesie (false = brak zapisanej, użyto domyślnych)
    bool load();

    // Zapisuje pełną konfigurację do NVS (przeżywa `uploadfs`, w odróżnieniu od LittleFS)
    bool save();

    // Zapisuje tylko rekordy dawkowania (doseRec[]) – szybkie call z loop()
    bool saveDoseRecords();

    // Przywraca wartości domyślne i zapisuje
    void resetToDefaults();

    // Zwraca JSON z konfiguracją (bez haseł) – do API GET /api/config
    String getConfigJson();

    // Aplikuje JSON z żądania POST /api/config i zapisuje
    bool applyConfigJson(const String& json);

} // namespace ConfigManager
