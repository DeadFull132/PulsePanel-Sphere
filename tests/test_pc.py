import json
import struct
import unittest
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'pc_background'))
from discovery import parse_reply, validate_ip
from rtss_reader import parse_rtss_v2
from telemetry import clamp_int

class PcUnitTests(unittest.TestCase):
    def test_local_ip(self):
        self.assertEqual(validate_ip('192.168.1.50'), '192.168.1.50')
        with self.assertRaises(ValueError): validate_ip('8.8.8.8')

    def test_discovery(self):
        packet = json.dumps({'product':'PulsePanel Sphere','version':7,'ip':'192.168.0.7'}).encode()
        self.assertEqual(parse_reply(packet, ('192.168.0.7', 1111)), '192.168.0.7')
        self.assertIsNone(parse_reply(packet, ('192.168.0.8', 1111)))

    def test_sensor_values(self):
        self.assertEqual(clamp_int(44.8, 0, 100), 45)
        self.assertIsNone(clamp_int(None, 0, 100))
        self.assertIsNone(clamp_int(float('nan'), 0, 100))

    def test_rtss_missing(self):
        self.assertIsNone(parse_rtss_v2(b'', {}, 3.0))
        header = struct.pack('<4sIIIIIIII', b'BAD!', 0x20000, 284, 36, 1, 0, 0, 0, 0)
        self.assertIsNone(parse_rtss_v2(header+b'\0'*284, {}, 3.0))

    def test_rtss_fps_and_stale(self):
        header = struct.pack('<4sIIIIIIII', b'RTSS', 0x20000, 284, 36, 1, 0, 0, 0, 0)
        record = struct.pack('<I260sIIIII', 1234, b'game.exe', 0, 500, 1500, 144, 6944)
        previous = {}
        d = parse_rtss_v2(header+record, previous, 10.0, 1234)
        self.assertEqual(d['fps'], 144)
        self.assertEqual(d['frametime_ms'], 6.944)
        self.assertEqual(d['game'], 'game.exe')
        self.assertIsNone(parse_rtss_v2(header+record, previous, 14.0, 1234))

if __name__ == '__main__': unittest.main()
