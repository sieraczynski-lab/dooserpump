# DooserPump API - mapowanie dla integracji z Home Assistant

Zweryfikowane 2026-09-12 bezpośrednio w kodzie (`src/web_server_handler.cpp`,
`src/mqtt_handler.cpp`, `src/config_manager.cpp`). DooserPump to PlatformIO/Arduino (nie ESPHome —
jak reefLamp, nie jak rollermat/pompa-dozujaca), ESP32-S3, 4-kanałowa pompa dozująca sterowana
silnikami BLDC z enkoderami (kalibracja impuls→ml zamiast czasu, dokładniejsza niż PWM+czas).

**Dwie różne, niepowiązane rzeczy w tym repo mają w nazwie "pompę dozującą" — nie mylić:**
`dooserpump/` (ten dokument, PlatformIO/Arduino, MQTT+HA Discovery) i `pompa-dozujaca/`
(ESPHome, bez MQTT — osobny dokument `pompa-dozujaca/docs/api-endpoints.md`).

Panel WWW (`data/index.html`) serwowany z LittleFS pod `/`, Basic Auth (domyślnie `admin`/`admin`,
zmień w konfiguracji). Adres urządzenia: sprawdź w routerze (DHCP) — świeże urządzenie startuje
w AP `ReefDosePump`/`reef1234` (`192.168.4.1`) dopóki nie skonfigurujesz WiFi przez panel.

## Dwa sposoby integracji

| Sposób | Port | Zalecane do |
|---|---|---|
| **MQTT + Home Assistant Discovery** | 1883 | Automatyczna integracja HA — zero ręcznej konfiguracji YAML, encje pojawiają się same |
| **REST API** (Basic Auth) | 80 | Node-RED, dashboardy niestandardowe, kalibracja, panel WWW |

MQTT tutaj jest **od razu gotowe pod HA** (w przeciwieństwie do rollermat) — publikuje
`homeassistant/.../config` discovery payloady automatycznie po połączeniu, HA tworzy encje bez
żadnej ręcznej konfiguracji `mqtt:` w YAML.

---

## MQTT (zalecane dla HA)

- Broker/port/user/hasło/topic bazowy — konfigurowalne przez panel WWW lub `POST /api/config`
  (pola `network.mqtt_host/mqtt_port/mqtt_user/mqtt_pass/mqtt_topic`), domyślny topic bazowy:
  `reef/dosepump`. **Puste `mqtt_host` = MQTT całkowicie wyłączone** (nie próbuje się łączyć).
- Client ID: `ReefDosePump_<mac_hex>`
- Reconnect co 10s jeśli rozłączone (tylko gdy WiFi połączone)

### Dostępność i stan

- `<topic>/status` = `"online"` / `"offline"` (LWT, retained) — `availability_topic` dla
  wszystkich encji Discovery
- `<topic>/pump/<idx>/state` (retained) — publikowane po każdej zmianie i przy połączeniu:
  ```json
  {"idx":0,"name":"Pompa 1","enabled":true,"running":false,
   "ml_today":12.5,"ml_total":340.2,"ml_day":20.0,"last_dose":"2026-09-12T14:30:00"}
  ```
  (`last_dose` pominięte w JSON, jeśli nigdy nie dawkowano)

### Komendy (subskrybowane przez urządzenie)

`<topic>/pump/<idx>/cmd` (idx = 0-3), payload JSON:

| `action` | Dodatkowe pole | Efekt |
|---|---|---|
| `dose` | `ml` (float, 0-50) | dawkuje `ml` mililitrów na kanale `idx` |
| `stop` | — | natychmiast zatrzymuje kanał |
| `enable` | — | włącza kanał (`pumpCal[idx].enabled = true`), zapisuje do NVS |
| `disable` | — | wyłącza kanał, zapisuje do NVS |
| `toggle` | — | odwraca `enabled` |

Przykład: `mosquitto_pub -t "reef/dosepump/pump/0/cmd" -m '{"action":"dose","ml":5.0}'`

**Uwaga:** komenda `dose` NIE sprawdza `enabled` po stronie MQTT tak samo rygorystycznie jak REST
(`if (ml > 0.0f && pumpCal[i].enabled)` — po prostu nic się nie stanie po cichu, bez błędu
zwrotnego, skoro MQTT nie ma odpowiedzi request/response). Sprawdź `<topic>/pump/<idx>/state`
(`running`) po wysłaniu komendy, jeśli potrzebujesz potwierdzenia.

### Home Assistant Discovery (automatyczne, per kanał)

Publikowane raz po połączeniu pod `homeassistant/<component>/reef_dosepump_<idx>/<object>/config`:

| Encja HA | Component | Źródło stanu |
|---|---|---|
| `<nazwa pompy> ml dziś` | sensor | `ml_today` z state topic |
| `<nazwa pompy> ml łącznie` | sensor | `ml_total` z state topic |
| `<nazwa pompy> aktywna` | switch | `enabled` z state topic, komenda `enable`/`disable` |
| `<nazwa pompy> dawkuj` | button | wysyła `dose` z domyślną dawką (`ml_day / doses_per_day`) |

Każda pompa to osobne "urządzenie" w HA (`identifiers: reef_dosepump_<idx>`) — nie ma potrzeby
ręcznej konfiguracji, wystarczy podłączyć broker w panelu WWW urządzenia.

---

## REST API (port 80, Basic Auth)

Autoryzacja: HTTP Basic (`netCfg.webUser`/`netCfg.webPass`, domyślnie `admin`/`admin`) —
wymagana na WSZYSTKICH `/api/*` i `/update`, NIE wymagana na `/health`.

### Status i konfiguracja

- `GET /api/status` — pełny status jednym zapytaniem:
  ```json
  {
    "fw":"1.0.0","device":"ReefDosePump","uptime":3600,"heap":180000,
    "time":"2026-09-12T14:30:00",
    "ap_mode":false,"wifi_ssid":"dom_IoT","wifi_ip":"192.168.30.X","wifi_rssi":-60,
    "mqtt_connected":true,
    "pumps":[{"idx":0,"name":"Pompa 1","enabled":true,"running":false,
              "ml_today":12.5,"ml_total":340.2,"ml_day":20.0,"doses":4,
              "last_dose":"2026-09-12T14:30:00"}, ...]
  }
  ```
- `GET /api/config` — pełna konfiguracja **bez haseł** (`network`, `general`, `pumps[]` — pola
  jak w tabeli niżej)
- `POST /api/config` **[JSON body]** — częściowa aktualizacja, dowolny podzbiór pól:
  ```json
  {"network":{"mqtt_host":"192.168.30.10","mqtt_topic":"reef/dosepump"},
   "pumps":[{"idx":0,"ml_day":25.0,"doses":6,"enabled":true}]}
  ```
  **Hasła** (`network.pass`, `network.mqtt_pass`, `network.web_pass`) aktualizowane **tylko gdy
  pole jest niepuste** w żądaniu — bezpieczne wysyłanie configu bez podawania haseł ponownie
  (ten sam wzorzec co naprawiony niedawno bug w reefLamp — tutaj zrobiony poprawnie od początku).

  Pola `network`: `ssid, pass, ip, gw, sn, mqtt_host, mqtt_port, mqtt_user, mqtt_pass,
  mqtt_topic, web_user, web_pass, configured`
  Pola `general`: `interval_min, ntp`
  Pola `pumps[]` (wymaga `idx` 0-3): `name, ppm (pulsesPerMl), ml_day, doses, enabled,
  win_start, win_end` (godziny 0-23/1-24, okno dawkowania)

### Sterowanie pompami

- `POST /api/pump/{idx}/dose` **[JSON body]** `{"ml": 2.5}` — dawkuje natychmiast (walidacja:
  `0 < ml ≤ 50`, wymaga `enabled=true` inaczej `409`)
- `POST /api/pump/{idx}/stop` — natychmiastowy stop kanału

### Kalibracja (impulsy enkodera → ml, dokładniejsza niż czas+PWM)

- `POST /api/calibrate/{idx}/start` — uruchamia pompę na pełnym PWM (duty 700/1023), zeruje
  licznik impulsów
- `POST /api/calibrate/{idx}/finish` **[JSON body]** `{"ml": X}` — zatrzymuje pompę, liczy
  `pulsesPerMl = pulsy / ml`, zapisuje do NVS. Zwraca `{"ok":true,"pulses":N,"ml":X,"ppm":Y}`.
  Błąd `409`, jeśli enkoder nie zarejestrował żadnych impulsów.
- `GET /api/pulses/{idx}` — aktualny surowy licznik impulsów (do podglądu na żywo w trakcie
  kalibracji, bez zatrzymywania pompy)

### Zarządzanie urządzeniem

- `POST /api/restart` — restart bez zmiany configu
- `POST /api/reset` — **kasuje całą konfigurację NVS** (WiFi, MQTT, kalibracja, rekordy) i
  restartuje — urządzenie wraca do trybu AP
- `POST /update` **[multipart/form-data]**, pole `firmware` = plik `.bin` — OTA firmware LUB
  filesystem (rozpoznawane po nazwie pliku: zawiera `littlefs`/`spiffs` → filesystem, inaczej
  firmware). **Konfiguracja (WiFi/MQTT/kalibracja) jest w NVS, osobnej partycji — przetrwa OTA
  filesystem bez zmian** (w przeciwieństwie do starszych projektów w tym repo, gdzie to był
  realny problem — tu zaprojektowane poprawnie od początku, patrz komentarz w `config.h`).

### Diagnostyka bez autoryzacji

- `GET /health` — celowo **bez Basic Auth**, żeby dało się zdiagnozować problem z siecią/panelem
  nawet gdy reszta nie odpowiada:
  ```json
  {"fw":"1.0.0","uptime_s":3600,"free_heap":180000,"min_free_heap":150000,
   "ap_mode":false,"wifi_connected":true,"wifi_ssid":"dom_IoT",
   "sta_ip":"192.168.30.X","ap_ip":"192.168.4.1"}
  ```

### WebSocket log (podgląd na żywo)

`ws://<ip>/ws/log` — strumień ostatnich ~60 linii logu (ring-buffer) + kolejne na bieżąco,
zwykły tekst per wiadomość. Przydatne przy diagnozowaniu OTA/MQTT na żywo bez portu szeregowego.

---

## Bezpieczeństwo dawkowania (dotyczy REST i MQTT jednakowo)

- Ręczna dawka ograniczona do **0 < ml ≤ 50** po stronie REST (`_handleDose`) — **MQTT `dose` nie
  ma górnego limitu wbudowanego w handler**, tylko sprawdza `ml > 0.0f && enabled` — jeśli
  budujesz automatyzację w HA, sam pilnuj rozsądnych wartości.
- Pompa wyłączona (`enabled=false`) blokuje dawkowanie na obu ścieżkach.
- Kalibracja przez impulsy enkodera (nie czas) — dozowanie kończy się, gdy licznik impulsów
  osiągnie cel (`ml × pulsesPerMl`), dokładniejsze przy zmiennym obciążeniu/napięciu niż typowe
  PWM+czas w innych projektach tego repo.
- **Timeout bezpieczeństwa per dawka** (`pump_controller.cpp`): szacowany czas dawki (przy
  nominalnym przepływie ~2 ml/s) razy 3 jako margines, przycięty do zakresu **10s-5min** — jeśli
  enkoder nie zarejestruje docelowej liczby impulsów w tym czasie (np. zapchana rurka, silnik
  zablokowany, enkoder padł), pompa zatrzymuje się awaryjnie i loguje `TIMEOUT`. Częściowo
  dostarczona objętość (na podstawie faktycznych impulsów) i tak dolicza się do `ml_today`/
  `ml_total` — dobowy limit nigdy nie pomija częściowo wykonanej dawki.
- Nowa komenda `dose` na kanale, który już dozuje, **przerywa poprzednią** (loguje jako
  "przerwana nową dawką") i rozlicza jej częściowy postęp, zamiast być ignorowana lub kolejkowana.
- Brak globalnego "cooldown" między kolejnymi dawkami na tym samym kanale (w przeciwieństwie do
  rollermat/pompa-dozujaca, gdzie jest to jawnie skonfigurowane) — jeśli potrzebujesz odstępu
  minimalnego, obecnie trzeba go wymusić po stronie automatyzacji HA/MQTT, nie ma tego w firmware.
