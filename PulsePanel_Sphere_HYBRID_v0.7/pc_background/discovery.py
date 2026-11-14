"""Local UDP discovery, HTTP status verification and private-LAN-only telemetry."""
from __future__ import annotations
import ipaddress
import json
import socket
import time
import urllib.request

PORT = 38555
MAGIC = b'PULSEPANEL_DISCOVER_V1'


def validate_ip(text):
    addr = ipaddress.IPv4Address(text)
    if not (addr.is_private or addr.is_loopback) or addr.is_link_local or addr.is_multicast:
        raise ValueError('To nie jest adres lokalny')
    return str(addr)


def parse_reply(data, sender):
    try:
        payload = json.loads(data)
        address = validate_ip(sender[0])
        if (payload.get('product') == 'PulsePanel Sphere' and
            payload.get('version') in (5, 6, 7) and payload.get('ip') == address):
            return address
    except (KeyError, ValueError, TypeError, AttributeError, UnicodeError):
        pass
    return None


def targets():
    found = {'255.255.255.255'}
    try:
        import psutil
        for interfaces in psutil.net_if_addrs().values():
            for item in interfaces:
                if item.family == socket.AF_INET and item.broadcast:
                    try:
                        found.add(validate_ip(item.broadcast))
                    except ValueError:
                        pass
    except Exception:
        pass
    return sorted(found)


OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http_status(ip):
    ip = validate_ip(ip)
    with OPENER.open(f'http://{ip}/api/v1/status', timeout=1.0) as response:
        data = json.loads(response.read(512))
    return data.get('product') == 'PulsePanel Sphere' and data.get('version') in (5, 6, 7) and data.get('wifi') is True


def discover(timeout=2.0):
    results = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.bind(('0.0.0.0', 0))
        for t in targets():
            try:
                s.sendto(MAGIC, (t, PORT))
            except OSError:
                pass
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            s.settimeout(max(.05, end-time.monotonic()))
            try:
                raw, remote = s.recvfrom(512)
            except socket.timeout:
                break
            except OSError:
                break
            ip = parse_reply(raw, remote)
            if ip and ip not in results:
                try:
                    if http_status(ip):
                        results.append(ip)
                except Exception:
                    continue
    return results


def send(ip, pin, payload):
    ip = validate_ip(ip)
    if not (isinstance(pin, str) and len(pin) == 8 and pin.isascii() and pin.isdigit()):
        raise ValueError('Kod parowania ma 8 cyfr')
    body = json.dumps(payload, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode('ascii')
    if len(body) > 510:
        raise ValueError('Pakiet za duzy')
    req = urllib.request.Request(f'http://{ip}/api/v1/metrics', data=body, method='POST',
        headers={'Content-Type':'application/json','X-PulsePanel-PIN':pin})
    with OPENER.open(req, timeout=2.5) as response:
        return json.loads(response.read(128)).get('ok') is True
