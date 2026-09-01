#pragma once
#include "config.h"

namespace ConfigManager {

    // Montuje LittleFS, tworzy domyślną konfigurację jeśli brak pliku
    void begin();

    // Wczytuje konfigurację z CONFIG_FILE → zwraca true przy sukcesie
    bool load();

    // Zapisuje pełną konfigurację do CONFIG_FILE
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
