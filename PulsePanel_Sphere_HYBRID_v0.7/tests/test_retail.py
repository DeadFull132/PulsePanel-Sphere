import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
class RetailChecks(unittest.TestCase):
    def test_portal_and_dns_exist(self):
        c=(ROOT/'firmware/main/main.c').read_text()
        self.assertIn('captive_dns_task',c)
        self.assertIn('captive_http_404',c)
        self.assertIn('CAPTIVE_DNS_PORT 53',c)
        self.assertIn('http://192.168.4.1/',c)
        self.assertIn('ESP_ERROR_CHECK(httpd_register_err_handler',c)
    def test_customer_wifi_reset_has_two_taps(self):
        c=(ROOT/'firmware/main/main.c').read_text()
        self.assertIn('wifi_reset_armed_until',c)
        self.assertIn('nvs_erase_key(h, "ssid")',c)
        self.assertIn('nvs_erase_key(h, "pass")',c)
        self.assertIn('POTWIERDZ',c)
    def test_pc_measurements_are_optional(self):
        docs=(ROOT/'INSTRUKCJA_DLA_KUPUJACEGO.md').read_text()
        self.assertIn('wymagająca modułu monitorującego',docs)
        self.assertIn('http://192.168.4.1',docs)
    def test_install_manifest_is_not_pretend_release(self):
        self.assertFalse((ROOT/'installer/manifest.json').exists())
        self.assertFalse((ROOT/'installer/pulsepanel-sphere-full.bin').exists())
if __name__ == '__main__': unittest.main()
