import unittest
from pathlib import Path
import json
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'pc_background'))
from discovery import parse_reply, validate_ip

class HybridChecks(unittest.TestCase):
    def setUp(self):
        self.fw = (ROOT / 'firmware/main/main.c').read_text(encoding='utf-8')
        self.pr = (ROOT / 'firmware/main/printer_monitor.c').read_text(encoding='utf-8')
        self.iss = (ROOT / 'pc_background/PulsePanelPC.iss').read_text(encoding='utf-8')

    def test_printer_optional_touch_wizard_and_persistence(self):
        for token in ('PAGE_PRINTER_WELCOME', 'printer_yes_press', 'printer_no_press',
                      '"pr_choice"', 'DODAJ / ZMIEN', 'printer_setup_page',
                      '/printer', 'printer_monitor_store'):
            self.assertIn(token, self.fw)
        self.assertIn('nvs_get_str(nvs,"pr_ip"', self.pr)
        self.assertIn('nvs_get_str(nvs,"pr_serial"', self.pr)
        self.assertIn('nvs_get_str(nvs,"pr_code"', self.pr)
        self.assertIn('nvs_commit(nvs)', self.pr)

    def test_touch_required_for_printer_config(self):
        self.assertIn('printer_config_until_us = esp_timer_get_time() + 600000000LL', self.fw)
        self.assertIn('printer_form_allowed()', self.fw)
        self.assertIn('!printer_form_allowed()', self.fw)
        self.assertIn('HTTPD_403_FORBIDDEN', self.fw)

    def test_printer_mqtt_read_only_and_local_lan(self):
        self.assertIn('mqtts://%s:8883', self.pr)
        self.assertIn('device/%s/report', self.pr)
        self.assertIn('"bblp"', self.pr)
        self.assertIn('pushall', self.pr)
        self.assertNotIn('"command\\":\\"stop', self.pr)
        self.assertNotIn('"command\\":\\"pause', self.pr)
        self.assertIn('lan_ip(address)', self.pr)
        self.assertIn('current_data_offset==0', self.pr)
        self.assertIn('last_report_us', self.pr)

    def test_reset_personal_data_before_shipping(self):
        for field in ('"ssid"','"pass"','"pc_pin"','"pr_choice"','"pr_ip"','"pr_serial"','"pr_code"'):
            self.assertIn(field, self.fw)
        self.assertIn('factory_reset_armed_until', self.fw)
        self.assertIn('RESET FABRYCZNY', self.fw)

    def test_windows_installer_is_silent_background_with_autostart(self):
        self.assertIn('PulsePanelPC_Background.exe', self.iss)
        self.assertIn('PulsePanelPC_Setup.exe', self.iss)
        self.assertIn('Parameters: "--pair"', self.iss)
        self.assertIn('CurrentVersion\\Run', self.iss)
        self.assertIn('PrivilegesRequired=lowest', self.iss)
        workflow=(ROOT/'.github/workflows/build-pc-agent.yml').read_text(encoding='utf-8')
        self.assertIn('--noconsole', workflow)
        self.assertIn('ISCC.exe', workflow)

    def test_discovery_accepts_hybrid_release(self):
        packet=json.dumps({'product':'PulsePanel Sphere','version':7,'ip':'192.168.10.31'}).encode()
        self.assertEqual(parse_reply(packet,('192.168.10.31',9999)), '192.168.10.31')
        self.assertIsNone(parse_reply(packet,('192.168.10.32',9999)))
        with self.assertRaises(ValueError): validate_ip('1.1.1.1')

    def test_no_fake_artifacts_in_zip_source(self):
        self.assertFalse((ROOT/'installer/manifest.json').exists())
        self.assertFalse((ROOT/'installer/pulsepanel-sphere-full.bin').exists())
        self.assertFalse((ROOT/'dist/installer/PulsePanelPC_Installer_v0.7.exe').exists())

if __name__ == '__main__': unittest.main()
