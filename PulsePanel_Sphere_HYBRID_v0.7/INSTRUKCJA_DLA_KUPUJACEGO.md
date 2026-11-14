# PulsePanel Sphere — instrukcja dla kupującego (prototyp)

**1. Włącz urządzenie.** Podłącz do zasilania przez USB. Przy pierwszym starcie ekran pokaże sieć `PulsePanel-XXXX` i hasło.

**2. Połącz Wi-Fi.** Telefonem połącz się z siecią monitorka i otwórz `http://192.168.4.1`, jeśli nie pojawi się strona automatycznie. Podaj domową sieć Wi-Fi 2,4 GHz. Po restarcie PulsePanel sam łączy się z routerem.

**3. Czy masz drukarkę Bambu Lab?** Na ekranie dotykowym wybierz **TAK** lub **NIE**.

- **NIE:** pomijasz konfigurację drukarki. Nadal możesz korzystać z zegara, Focus, dźwięku i innych ekranów. Możesz dodać drukarkę później na stronie Bambu Lab → `DODAJ / ZMIEN`.
- **TAK:** monitor pokaże lokalny adres strony (np. `http://192.168.1.80/printer`). Otwórz go na telefonie w tej samej sieci i wpisz IP drukarki, jej numer seryjny i kod dostępu LAN z ustawień drukarki. Zatwierdź. Urządzenie zapamięta drukarkę i będzie próbowało łączyć się przy kolejnych uruchomieniach. Nie każda konfiguracja Bambu Lab umożliwia taki dostęp; zależy to od modelu i firmware.

**4. Sterowanie dotykiem.** Przesuń palcem w lewo/prawo, aby wybrać zegar, Gaming PC, Focus, test głośnika, ustawienia sieci lub drukarkę Bambu Lab. Wybór nie wymaga uruchamiania panelu na komputerze.

**5. Monitoring PC (opcjonalnie).** Aby na BambuSphere pojawiały się FPS i statystyki Windows, konieczna jest instalacja **jednorazowa** małego nadajnika Windows — oprogramowania działającego po instalacji bez okna. Uruchom udostępniony przez sprzedawcę `PulsePanelPC_Installer_v0.7.exe` i wprowadź kod PC wyświetlony na stronie `SIEC` monitorka. FPS wymaga też źródła pomiaru RTSS; bez niego wyświetlany jest `--`. To funkcja wymagająca modułu monitorującego PC — samo Wi-Fi nie udostępnia FPS.

**6. Zmiana Wi-Fi i danych.** Na stronie `SIEC` dotknij `ZMIEN WI-FI` i potwierdź. Przy przekazaniu urządzenia komuś innemu użyj `RESET FABRYCZNY` (podwójne potwierdzenie), aby usunąć Wi-Fi, sparowany komputer i drukarkę. Nie udostępniaj nikomu kodu dostępu LAN ani PIN-u PC.

Niektóre funkcje i czujniki zależą od wersji urządzenia oraz środowiska sieciowego. To dokument prototypu, który wymaga weryfikacji na rzeczywistym sprzęcie przed dołączaniem do sprzedawanego produktu.
