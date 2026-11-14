import json
import tempfile
import unittest
from pathlib import Path
from tools.package_installer import prepare

class PackageTests(unittest.TestCase):
    def test_missing_binary_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(RuntimeError):
                prepare(Path(d) / 'absent.bin', Path(d) / 'site')

    def test_tiny_binary_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'firmware.bin'
            src.write_bytes(b'a' * 50)
            with self.assertRaises(RuntimeError):
                prepare(src, Path(d) / 'site')

    def test_compiled_image_creates_correct_manifest(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 'image.bin'
            src.write_bytes(b'\xe9' + b'\xff' * 100_000)
            target = prepare(src, Path(d) / 'site')
            m = json.loads((Path(d) / 'site' / 'manifest.json').read_text())
            self.assertTrue(target.exists())
            self.assertEqual(m['builds'][0]['chipFamily'], 'ESP32-S3')
            self.assertEqual(m['builds'][0]['parts'][0]['offset'], 0)
            self.assertEqual(m['builds'][0]['parts'][0]['path'], target.name)

if __name__=='__main__': unittest.main()
