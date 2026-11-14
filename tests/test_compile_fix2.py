import unittest
from pathlib import Path

class CompileFix2Tests(unittest.TestCase):
    def test_wifi_password_copy_is_bounded(self):
        src = (Path(__file__).resolve().parent.parent / 'firmware/main/main.c').read_text(encoding='utf-8')
        self.assertIn('strnlen(password, sizeof(cfg.sta.password))', src)
        self.assertIn('memcpy(cfg.sta.password, password, wifi_password_len)', src)
        self.assertNotIn('snprintf((char *)cfg.sta.password', src)

if __name__ == '__main__': unittest.main()
