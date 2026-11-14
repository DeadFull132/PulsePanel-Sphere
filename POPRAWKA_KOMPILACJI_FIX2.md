# PulsePanel Sphere v0.7 — BUILD FIX 2

## Dlaczego GitHub Actions był czerwony?

ESP-IDF 5.5.6 traktował jako błąd ostrzeżenie `-Werror=format-truncation`:

`firmware/main/main.c:913` — `snprintf` zapisywał hasło do 64-bajtowej tablicy `cfg.sta.password`.

## Zmiana

Zastąpiono kopiowanie przez `snprintf` następującymi instrukcjami:

```c
size_t wifi_password_len = strnlen(password, sizeof(cfg.sta.password));
memcpy(cfg.sta.password, password, wifi_password_len);
```

Kod respektuje rozmiar bufora (64 bajty) i nie usuwa obsługi 64-znakowego klucza WPA PSK.

Poprzednie poprawki `printer_monitor.c` i kodów HTTP z FIX1 pozostają bez zmian.

## Jak zaktualizować repozytorium GitHub Desktop

1. Rozpakuj tę paczkę do nowego folderu.
2. W GitHub Desktop wybierz repozytorium `PulsePanel-Sphere`.
3. W menu wybierz `Repository -> Show in Explorer`.
4. Skopiuj **z nowej paczki** plik `PulsePanel_Sphere_HYBRID_v0.7/firmware/main/main.c` do `firmware/main/main.c` w repozytorium (potwierdź zamianę).
5. W GitHub Desktop wpisz `Fix: haslo WiFi ESP-IDF` jako Summary, kliknij `Commit to main`, następnie `Push origin`.
6. Na GitHub przejdź do `Actions` i zobacz wynik nowego uruchomienia.

## Stan testów

Testy Python potwierdzają wcześniejsze funkcjonalności i format paczki. Ta poprawka nie została jeszcze sprawdzona przez pełną kompilację ESP-IDF ani na rzeczywistym urządzeniu. Sukces GitHub Actions nie jest testem działania na BambuSphere.

**Nie wgrywaj na BambuSphere przed weryfikacją sprzętu i wykonaniem kopii firmware.**
