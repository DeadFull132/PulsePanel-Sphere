# PulsePanel Sphere HYBRID v0.7 — projekt prototypowy

**STATUS: źródła, NIE gotowy przetestowany produkt.** Pliki zostały przeanalizowane i testowane na poziomie Pythona, ale **nie skompilowano ani nie sprawdzono na fizycznym ESP32-S3** firmware i nie zbudowano tu instalatora EXE dla Windows. Nie wgrywaj do urządzenia sprzedawanego klientowi bez własnej kompilacji i testów. Pliki `.bin` i `.exe` **nie znajdują się w ZIP**. Dotyczy Waveshare ESP32-S3-Touch-AMOLED-1.75, okrągły 466 × 466 (nie 1.75C).

## Co jest nowe

1. **Bezużytkownikowa aplikacja Windows**: po jednorazowym sparowaniu proces `PulsePanelPC_Background.exe` uruchamia się bez okna przy zalogowaniu. Nie służy do przełączania ekranów ani ustawień — te są obsługiwane dotykiem na urządzeniu. Instalator Windows należy najpierw zbudować w GitHub Actions: `.github/workflows/build-pc-agent.yml` (PyInstaller + Inno Setup 6).
2. **Dodanie drukarki Bambu Lab jest opcjonalne**: po pierwszym podłączeniu Wi-Fi ekran pyta `MASZ DRUKARKE BAMBU LAB? TAK / NIE`. `NIE` pomija temat. `TAK` otwiera ekran z adresem strony konfiguracji na telefonie. Formularz prosi o lokalny adres IP, numer seryjny i kod LAN. Zapis w NVS monitorka. Po restarcie firmware sam próbuje odnawiać połączenie MQTT po lokalnej sieci, także bez PC.
3. **Ekran drukarki**: stan, procent postępu, nazwa zadania, szacowany pozostały czas, temperatury stołu i dyszy, warstwy, numer wybranego slotu AMS, jeśli raportowany. Każde pole może być niedostępne na danym modelu/firmware. Komunikacja z drukarką jest **tylko do odczytu**. Kod nie wysyła `pause`, `stop`, `resume`, nie rozpoczyna druku, nie obsługuje podglądu kamery i nie kopiuje interfejsu BambuSphere.
4. **Zmiana ustawień z monitorka**: gest poziomy przełącza zegar / Gaming PC / Focus / Audio / Sieć / Bambu Lab. Na ekranie drukarki znajduje się przycisk `DODAJ / ZMIEN`. Dane logowania do drukarki wpisuje się na telefonie jednorazowo — na małym ekranie nie ma klawiatury.
5. **Reset przed sprzedażą**: w zakładce Sieć `RESET FABRYCZNY` (potwierdzenie drugim dotknięciem w 8 sekund) usuwa Wi-Fi, PIN PC, dane drukarki i odpowiedź TAK/NIE. `ZMIEN WI-FI` zachowuje drukarkę oraz PIN PC.

## Instrukcja kupującego — bez instalacji dla funkcji samodzielnych

1. Podłącz zasilanie USB. Przy pierwszym uruchomieniu połącz telefon z siecią `PulsePanel-XXXX` (hasło na ekranie).
2. Wpisz domowe Wi-Fi 2,4 GHz na stronie wyświetlonej automatycznie lub `http://192.168.4.1`. Monitorek i drukarka/PC powinny być w tej samej sieci (bez izolacji klientów).
3. Po pierwszym połączeniu PulsePanel zapyta o drukarkę Bambu Lab. Kliknij `NIE` albo `TAK`. Dla `TAK` otwórz wskazany adres `http://<IP-PulsePanel>/printer` na telefonie połączonym z domowym Wi-Fi i podaj IP drukarki, numer seryjny i kod LAN. Dane zostaną zapamiętane, a monitorek uruchomi się ponownie.
4. Dotykiem przełączaj zegar, Focus, dźwięki, Gaming PC i Bambu Lab. Jeśli drukarka nie jest dostępna, pojawia się `DRUKARKA OFFLINE`, a nie sztuczne dane.

**Ważne:** samodzielne funkcje i monitoring Bambu Lab nie wymagają instalowania czegokolwiek na PC. Jeżeli klient chce widzieć **statystyki systemu Windows lub FPS**, musi zainstalować poniższy opcjonalny nadajnik. Żadne urządzenie ESP32 nie odczyta FPS z gry PC bez zewnętrznego źródła.

## Opcjonalny instalator Windows — z punktu widzenia klienta

1. Producent buduje `PulsePanelPC_Installer_v0.7.exe` w GitHub Actions z `.github/workflows/build-pc-agent.yml` i udostępnia klientom wyłącznie ten **jeden plik instalacyjny**.
2. Kupujący uruchamia instalator. Po zainstalowaniu jednorazowo otworzy się proste okno tekstowe parowania — wykrywa PulsePanel w lokalnej sieci, a gdy to nie działa, prosi o IP.
3. W zakładce `SIEC` monitorka wyświetla się 8-cyfrowy kod `KOD PC`. Kupujący wpisuje go raz podczas parowania.
4. Od tej pory `PulsePanelPC_Background.exe` startuje bez okna przy logowaniu Windows, wysyłając dane co około sekundę. Na monitorze komputera nie ma osobnego pulpitu PulsePanel. Aby zmienić ekran, użyj dotyku na PulsePanel.
5. Wartości CPU/RAM zazwyczaj są dostępne; GPU/temperatury zależą od sterowników i czujników. **FPS i przybliżony 1% Low wymagają obecnie RTSS**, osobnego źródła danych FPS — instalator tego nie zastępuje i nie zawiera RTSS. Brak FPS jest pokazywany jako `--`, a nie wymyślona liczba.

## Instalator firmware w przeglądarce — dla producenta

- Zainstaluj ESP-IDF 5.5.x. W `firmware/` uruchom `idf.py set-target esp32s3`, `idf.py build`, `idf.py merge-bin -o build/pulsepanel-sphere-full.bin -f raw`.
- `python tools/package_installer.py` przygotuje `installer/manifest.json` i skopiuje prawdziwy skompilowany obraz (odrzuca nieistniejący i bardzo mały plik).
- Strona `installer/index.html` jest do umieszczenia pod HTTPS (GitHub Pages) i korzysta z ESP Web Tools / WebSerial w Chrome lub Edge. GitHub workflow `.github/workflows/build-and-pages.yml` automatyzuje kompilację i publikację po uruchomieniu.
- **Wersja z ZIP nie zawiera skompilowanego firmware**. Najpierw wykonaj backup istniejącej pamięci za pomocą esptool (dopasuj rozmiar do realnej płytki); flashowanie nadpisuje aktualne BambuSphere.

## Połączenie z drukarką — zasady techniczne

- Lokalny MQTT przez TLS: `mqtts://<IP-drukarki>:8883`, login `bblp`, hasło = kod dostępu LAN, raporty z `device/<serial>/report`. `pushall` jedynie prosi o pełen stan, nie steruje wydrukiem.
- Dostęp i format raportów są zależne od modelu i wersji firmware Bambu Lab; nowsze wersje z Authorization Control mogą blokować integracje. Opcja LAN Only/Developer Mode może być wymagana, ale nie włączaj jej bez sprawdzenia skutków na swoim modelu. Część trybów wyłącza usługi chmurowe.
- Wystarczy połączenie do domowej sieci, bez konta Bambu Cloud i bez komputera, **o ile lokalny odczyt MQTT jest dostępny**. Numer IP drukarki może się zmienić; zalecamy rezerwację DHCP w routerze.
- Komunikacja z drukarką jest szyfrowana przez TLS, ale w tym prototypie **nie weryfikuje jej certyfikatu** (drukarki używają certyfikatów własnych). Credentials drukarki są przechowywane jako tekst w NVS urządzenia, a formularz parowania używa lokalnego **HTTP, nie HTTPS**. Nie używaj na niezaufanej sieci; przed sprzedażą wykonaj przegląd bezpieczeństwa, włącz szyfrowanie NVS, rozważ przypięcie certyfikatu i zabezpiecz konfigurację.

## Co trzeba dokończyć przed sprzedażą

- Udana kompilacja firmware i test na realnej płytce AMOLED: dotyk, jasność, portal Wi-Fi, dźwięk i stabilność.
- Test połączenia z przynajmniej jednym modelem drukarki Bambu Lab, w tym zanik i ponowne połączenie, przypadek zmienionego IP, działanie AMS.
- Test Windows instalatora EXE na czystym Windows 10/11 oraz autostartu, wyłączenia i odinstalowania.
- Sposób dystrybucji aktualizacji firmware; reset danych klienta przed wysyłką.
- Sprawdzenie licencji bibliotek, modeli 3D/obudów oraz wymagań prawnych sprzedaży elektroniki. Kod jest autorskim prototypem i **nie zawiera kodu BambuSphere/PrintSphere o licencji niekomercyjnej**.

## Struktura

`firmware/main/main.c` — dotyk, sieć, Wi-Fi captive portal, Windows telemetry, wybór drukarki.
`firmware/main/printer_monitor.c/.h` — niezależny odbiornik lokalnego MQTT Bambu Lab.
`pc_background/` — program Windows w tle, odczytywanie statystyk, plik źródłowy instalatora Inno Setup.
`.github/workflows/` — budowanie instalatora Windows i firmware.
`installer/` — szablon strony WebSerial.
`INSTRUKCJA_DLA_KUPUJACEGO.md` — prosta instrukcja; `SPRZEDAWCA_CHECKLISTA.md` — lista przed wysyłką.

**Stan techniczny: prototyp do dalszych testów. Nie jest to zatwierdzona wersja produkcyjna.**
