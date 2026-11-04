import tempfile
from pathlib import Path
import unittest
import zlib

from scripts.run_scenarios import load_scenarios, status_matches, validate_step, corrupt_settings_crc


class ScenarioValidationTests(unittest.TestCase):
    def test_bad_steps_are_rejected(self):
        steps = [
            {}, {"command": "start"}, {"command": "start\nstop", "expect": ["OK"]},
            {"command": "start", "expect": ["OK"], "restart": True},
            {"status": {}}, {"status": {"unknown": 1}},
            {"status": {"state": "READY"}, "timeout_ms": 0},
            {"wait_ms": -1}, {"wait_ms": True}, {"restart": False}, {"corrupt_settings_crc": False},
            {"corrupt_settings_crc": True, "restart": True},
            {"command": "start", "expect": [""]},
        ]
        for step in steps:
            with self.subTest(step=step), self.assertRaises(ValueError):
                validate_step(step)

    def test_empty_scenario_file_is_not_a_passing_run(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "empty.yaml"
            path.write_text("version: 1\nscenarios: []\n")
            with self.assertRaises(ValueError):
                load_scenarios(path)

    def test_duplicate_keys_and_boolean_version_are_rejected(self):
        examples = [
            "version: true\nscenarios: [{name: a, steps: [{restart: true}]}]\n",
            "version: 1\nversion: 1\nscenarios: [{name: a, steps: [{restart: true}]}]\n",
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "invalid.yaml"
            for text in examples:
                path.write_text(text)
                with self.assertRaises(ValueError):
                    load_scenarios(path)

    def test_status_checks_complete_values(self):
        self.assertFalse(status_matches("active=0x10", {"active": 1}))
        self.assertTrue(status_matches("state=FAULT active=0x2 output_x100=0", {
            "state": "FAULT", "active": 2, "output_x100": 0}))
        self.assertFalse(status_matches("state=READY", {"output_x100": 0}))

    def test_crc_injection_changes_only_inner_crc_and_repairs_nvs_crc(self):
        blob = bytearray(b"1CIZ" + (1).to_bytes(4, "little") + bytes(88))
        blob += zlib.crc32(blob).to_bytes(4, "little")
        image = b"prefix" + blob + zlib.crc32(blob).to_bytes(4, "little")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "controller_flash.bin")
            path.write_bytes(image)
            corrupt_settings_crc(directory)
            changed = path.read_bytes()
            self.assertEqual(changed[:102], image[:102])
            self.assertNotEqual(changed[102:106], image[102:106])
            self.assertEqual(zlib.crc32(changed[6:106]), int.from_bytes(changed[106:110], "little"))
            with self.assertRaises(ValueError):
                corrupt_settings_crc(directory)

    def test_crc_injection_rejects_missing_truncated_and_ambiguous_records(self):
        marker = b"1CIZ" + (1).to_bytes(4, "little")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "controller_flash.bin")
            for image in (b"", marker, marker + bytes(104) + marker + bytes(104)):
                path.write_bytes(image)
                with self.assertRaises(ValueError):
                    corrupt_settings_crc(directory)
                self.assertEqual(path.read_bytes(), image)

    def test_repository_scenarios_are_valid(self):
        scenarios = load_scenarios("scenarios/controller.yaml")
        self.assertGreaterEqual(len(scenarios), 6)


if __name__ == "__main__":
    unittest.main()
