#pragma once

namespace WebServerHandler {

    // Uruchamia AsyncWebServer na porcie 80
    void begin();

    // Wywołuj w loop() – czyści nieaktywnych klientów WebSocket
    void loop();

    // Wysyła tekst do wszystkich podłączonych klientów WS (bez Serial)
    void wsLog(const char* msg);

    // Formatuje jak printf, wypisuje na Serial i przez WebSocket
    void wsLogf(const char* fmt, ...);

} // namespace WebServerHandler
