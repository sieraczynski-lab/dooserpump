#pragma once
#include <Arduino.h>

namespace MqttHandler {

    // Inicjalizuje klienta, łączy i publikuje HA Discovery
    void begin();

    // Wywołuj z loop() – utrzymuje połączenie, obsługuje wiadomości
    void loop();

    // Publikuje stan wszystkich pomp
    void publishAll();

    // Publikuje stan pojedynczej pompy
    void publishPump(uint8_t idx);

    // true jeśli klient MQTT jest połączony
    bool isConnected();

} // namespace MqttHandler
