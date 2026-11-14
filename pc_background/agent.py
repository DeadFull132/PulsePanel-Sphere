"""PulsePanel PC background sender. First launch: python agent.py --pair."""
from __future__ import annotations
import argparse
import json
import logging
import os
from pathlib import Path
import sys
import time
import urllib.error

from discovery import discover, send, validate_ip, http_status
from telemetry import Metrics


def config_path():
    root = Path(os.environ.get('APPDATA', str(Path.home()))) / 'PulsePanelSphere'
    root.mkdir(parents=True, exist_ok=True)
    return root / 'pc_agent.json'


def load_config():
    try:
        d = json.loads(config_path().read_text(encoding='utf-8'))
        if not isinstance(d, dict): return {}
        if d.get('pin') and (not str(d['pin']).isdigit() or len(d['pin']) != 8): return {}
        return d
    except (OSError, ValueError):
        return {}


def save_config(config):
    path = config_path()
    temp = path.with_suffix('.tmp')
    temp.write_text(json.dumps(config, indent=2), encoding='utf-8')
    temp.replace(path)


def pair():
    print('PulsePanel Sphere / jednorazowe parowanie komputera')
    print('BambuSphere i komputer musza byc w tej samej sieci Wi-Fi/LAN.')
    devices = discover(4.0)
    for i, address in enumerate(devices, 1): print(f'  {i}. {address}')
    if len(devices) == 1: ip = devices[0]
    elif devices:
        while True:
            choice = input('Numer urzadzenia: ').strip()
            if choice.isdecimal() and 1 <= int(choice) <= len(devices):
                ip = devices[int(choice)-1]; break
    else:
        print('Brak automatycznego wykrywania. Mozesz podac IP z ekranu BambuSphere.')
        ip = validate_ip(input('IP urzadzenia (np. 192.168.1.100): ').strip())
        if not http_status(ip): raise RuntimeError('Nie znaleziono zgodnego firmware v0.7 pod tym IP')
    print('Na BambuSphere przesun na ekran SIEC i odczytaj napis KOD PC.')
    pin = input('Wpisz 8-cyfrowy KOD PC: ').strip()
    if len(pin) != 8 or not pin.isascii() or not pin.isdigit():
        raise ValueError('Nieprawidlowy 8-cyfrowy kod parowania')
    if not send(ip, pin, {'v':1, 'fps':None, 'low':None, 'frame_x10':None,
                          'cpu':None, 'gpu':None, 'ram':None, 'cpu_temp':None,
                          'gpu_temp':None, 'game':'SPAROWANO'}):
        raise RuntimeError('Brak potwierdzenia parowania')
    save_config({'ip':ip, 'pin':pin})
    print('Sparowano. Dalej komputer bedzie laczyl sie automatycznie.')


def loop():
    data = load_config()
    if not data.get('pin'):
        raise RuntimeError('Brak parowania. Uruchom parowanie z instalatora Windows (tryb --pair)')
    monitor = Metrics()
    logging.info('PulsePanel background sender start')
    last_discovery = 0.0
    known = data.get('ip', '')
    while True:
        started = time.monotonic()
        payload = monitor.read()
        try:
            # Reuse saved IP while reachable. On router address changes, broadcast discover.
            if not known and time.monotonic() - last_discovery > 8:
                found = discover(1.8)
                last_discovery = time.monotonic()
                # Only pair to another IP if precisely one matching device is visible.
                known = found[0] if len(found) == 1 else None
            if known and send(known, str(data['pin']), payload):
                if known != data.get('ip'):
                    data['ip'] = known; save_config(data)
            else:
                known = None
        except (OSError, urllib.error.URLError, ValueError, TimeoutError) as exc:
            logging.warning('Brak polaczenia PC -> Sphere: %s', exc)
            known = None
        elapsed = time.monotonic() - started
        time.sleep(max(.3, 1.0 - elapsed))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--pair', action='store_true')
    parser.add_argument('--once', action='store_true')
    args = parser.parse_args()
    if args.pair:
        pair(); return
    logfile = config_path().parent / 'pc_agent.log'
    logging.basicConfig(filename=logfile, level=logging.INFO, format='%(asctime)s %(message)s')
    if args.once:
        config = load_config()
        if not config.get('pin'): raise RuntimeError('Najpierw sparuj BambuSphere')
        print(send(config['ip'], config['pin'], Metrics().read())); return
    while True:
        try: loop()
        except Exception:
            logging.exception('PC agent restart')
            time.sleep(10)


if __name__ == '__main__':
    try: main()
    except (KeyboardInterrupt, EOFError): sys.exit(1)
