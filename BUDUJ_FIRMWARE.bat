@echo off
setlocal
cd /d "%~dp0firmware"
where idf.py >nul 2>nul
if errorlevel 1 (
  echo Uruchom skrypt z konsoli ESP-IDF 5.5.6.
  pause
  exit /b 1
)
idf.py set-target esp32s3 || exit /b 1
idf.py build || exit /b 1
idf.py merge-bin -o build/pulsepanel-sphere-full.bin -f raw || exit /b 1
cd ..
python tools\package_installer.py || exit /b 1
echo Firmware skompilowane, pliki instalatora gotowe w folderze installer.
start "" "http://localhost:8000"
python -m http.server 8000 --directory installer
