"""Hardware metrics. Missing sensors remain null; no invented temperatures or FPS."""
from __future__ import annotations
import collections
import math
import os
import statistics
import time

try:
    import psutil
except ImportError:
    psutil = None

from rtss_reader import RTSSReader


def clamp_int(value, lo: int, hi: int):
    try:
        v = float(value)
        if math.isfinite(v) and lo <= v <= hi:
            return round(v)
    except (TypeError, ValueError, OverflowError):
        pass
    return None


class Metrics:
    def __init__(self):
        if psutil is None:
            raise RuntimeError('Brak psutil. Uruchom 00_INSTALUJ_PC.bat')
        psutil.cpu_percent(None)
        self.rtss = RTSSReader()
        self.samples = collections.deque(maxlen=120)  # observations, NOT a complete frame trace
        self.game = None
        self.last_game_change = 0.0
        self.nvml = None
        self.gpu_handle = None
        try:
            import pynvml
            pynvml.nvmlInit()
            self.nvml = pynvml
            self.gpu_handle = pynvml.nvmlDeviceGetHandleByIndex(0)
        except Exception:
            pass
        self.lhm = None
        if os.name == 'nt':
            try:
                import wmi
                self.lhm = wmi.WMI(namespace='root\\LibreHardwareMonitor')
            except Exception:
                pass

    def read_lhm(self):
        result = {}
        if not self.lhm:
            return result
        try:
            sensors = self.lhm.Sensor()
            for s in sensors:
                st = str(s.SensorType)
                name = (str(s.Name) + ' ' + str(getattr(s, 'Identifier', ''))).lower()
                if st == 'Temperature':
                    if ('cpu package' in name or 'cpu tctl/tdie' in name) and 'cpu_temp' not in result:
                        result['cpu_temp'] = clamp_int(s.Value, -30, 150)
                    if ('gpu core' in name or 'gpu temperature' in name) and 'gpu_temp' not in result:
                        result['gpu_temp'] = clamp_int(s.Value, -30, 150)
                if st == 'Load' and 'gpu core' in name and 'gpu' not in result:
                    result['gpu'] = clamp_int(s.Value, 0, 100)
        except Exception:
            pass
        return result

    def read(self):
        data = {'v': 1, 'fps': None, 'low': None, 'frame_x10': None,
                'game': None, 'cpu': clamp_int(psutil.cpu_percent(None), 0, 100),
                'ram': clamp_int(psutil.virtual_memory().percent, 0, 100),
                'gpu': None, 'cpu_temp': None, 'gpu_temp': None}
        frame = self.rtss.read()
        if frame:
            data['fps'] = frame['fps']
            data['game'] = frame['game']
            data['frame_x10'] = clamp_int(frame['frametime_ms'] * 10 if frame['frametime_ms'] is not None else None, 0, 30000)
            now = time.monotonic()
            if data['game'] != self.game or now - self.last_game_change > 4.0:
                if data['game'] != self.game:
                    self.samples.clear()
                self.game = data['game']
                self.last_game_change = now
            # A sampling approximation, NOT the mathematically exact frame-based 1% low.
            # Avoid showing it when we have fewer than 50 distinct observations.
            self.samples.append(frame['fps'])
            if len(self.samples) >= 50:
                slowest = sorted(self.samples)
                n = max(1, math.ceil(len(slowest) * 0.01))
                data['low'] = round(statistics.mean(slowest[:n]))
        else:
            self.samples.clear()
            self.game = None
        if self.nvml is not None:
            try:
                data['gpu'] = clamp_int(self.nvml.nvmlDeviceGetUtilizationRates(self.gpu_handle).gpu, 0, 100)
                data['gpu_temp'] = clamp_int(
                    self.nvml.nvmlDeviceGetTemperature(self.gpu_handle, self.nvml.NVML_TEMPERATURE_GPU), -30, 150)
            except Exception:
                pass
        extra = self.read_lhm()
        for key, value in extra.items():
            if data.get(key) is None:
                data[key] = value
        return data
