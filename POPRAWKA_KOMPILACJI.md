# PulsePanel Sphere HYBRID v0.7 — poprawka błędu kompilacji 1

Ta paczka jest pełnym projektem v0.7 z poprawką dla trzech błędów zgłoszonych przez GitHub Actions (ESP-IDF v5.5.6):

1. `firmware/main/printer_monitor.c`: dodano `#include "lwip/sockets.h"`, które udostępnia `AF_INET` i `inet_pton`.
2. `firmware/main/main.c`: dwukrotnie zastąpiono nieistniejące `HTTPD_503_SERVICE_UNAVAILABLE` wywołaniem `httpd_resp_send_custom_err(request, "503 Service Unavailable", ...)`.

## Jak aktualizować na GitHub Desktop
1. Rozpakuj tę paczkę.
2. W GitHub Desktop wybierz repozytorium `PulsePanel-Sphere` i menu **Repository → Show in Explorer**.
3. **Skopiuj ZAWARTOŚĆ** folderu `PulsePanel_Sphere_HYBRID_v0.7` do głównego folderu repozytorium (tam, gdzie leżą `.github`, `firmware`, `pc_background`). Wyraź zgodę na zastąpienie istniejących plików.
4. W GitHub Desktop wpisz `Poprawka kompilacji ESP-IDF` w polu Summary, kliknij **Commit to main**, potem **Push origin**.
5. GitHub Actions może uruchomić budowanie automatycznie po push. W innym przypadku wybierz Actions → Build firmware and publish web installer → Run workflow.
6. Jeśli nowy build jest czerwony, skopiuj pierwszy komunikat `error:` (poza długą linią kompilatora), aby ustalić następny błąd.

## Ograniczenia
- Testy Pythona (19) w paczce mogą przejść, ale **to nie jest weryfikacja kompilacji firmware**.
- Nie przetestowano tego na fizycznym ekranie i drukarce Bambu Lab.
- Nie wgrywaj firmware na swoje jedyne urządzenie bez kopii pamięci Flash i potwierdzenia dokładnego modelu.
