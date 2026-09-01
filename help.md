ledcAttach(pin, freq, res) + ledcWrite(pin, duty) — nowe API Arduino ESP32 3.x (IDF 5.x)
Enkodery przez attachInterrupt (RISING na kanale A) — portabilne, wystarczające dla niskich prędkości
Scheduler sprawdza co 10 s, dzieli mlPerDay / dosesPerDay, respektuje minimalny interwał
MQTT: retency messages, HA Discovery dla każdej pompy (sensor + switch + button)
Web UI: odświeżanie co 5 s, wbudowany kreator kalibracji z live licznikiem impulsów
Następne kroki:

pio run — skompiluj
pio run -t upload — wgraj firmware
pio run -t uploadfs — wgraj web UI na LittleFS

pio run -t clean - czyszczenie projektu

pio device monitor - logi z procesora

Połącz z AP ReefDosePump / hasło reef1234, wejdź na 192.168.4.1

# Najpierw wgraj nowe firmware (żeby endpoint obsługiwał oba typy)
platformio run --target upload

# Następnie filesystem (index.html i inne pliki z data/)
platformio run --target buildfs

OTA Update w Powershel
curl.exe -u admin:admin -F "firmware=@.pio\build\esp32-s3-zero\firmware.bin" http://192.168.0.14/update
curl.exe -u admin:admin -F "firmware=@.pio\build\esp32-s3-zero\littlefs.bin" http://192.168.0.14/update


