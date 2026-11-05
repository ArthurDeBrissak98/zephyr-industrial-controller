import tempfile
from pathlib import Path
import unittest
import zlib
from unittest.mock import patch

from scripts.run_scenarios import load_scenarios, status_matches, validate_step, corrupt_settings_crc, run_scenario
from scripts.modbus_client import Client, frame


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

    def test_log_lines_do_not_override_status_fields(self):
        output = "state=FAULT active=0x8\n[00:00:01] <err> controller_app: FAULT active=0x9\n"
        self.assertFalse(status_matches(output, {"state": "FAULT", "active": 9}))
        self.assertTrue(status_matches(output, {"state": "FAULT", "active": 8}))

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

    def test_modbus_step_validation(self):
        validate_step({"modbus_read": {"address": 0, "count": 5, "expect": [1, 0, 0, 500, 500]}})
        validate_step({"modbus_read": {"address": 10, "count": 5, "expect": [400, 11500, 5000, 1450, 2]}})
        for request in (
            {}, {"address": 0, "count": 1},
            {"address": True, "count": 1, "expect": [1]},
            {"address": 0, "count": True, "expect": [1]},
            {"address": 4, "count": 2, "expect": [0, 0]},
            {"address": 5, "count": 1, "expect": [0]},
            {"address": 9, "count": 2, "expect": [0, 0]},
            {"address": 14, "count": 2, "expect": [0, 0]},
            {"address": -1, "count": 1, "expect": [0]},
            {"address": 0, "count": 0, "expect": []},
            {"address": 0, "count": 1, "expect": [True]},
            {"address": 0, "count": 1, "expect": [-1]},
            {"address": 0, "count": 1, "expect": [65536]},
            {"address": 0, "count": 2, "expect": [1]},
        ):
            with self.subTest(request=request), self.assertRaises(ValueError):
                validate_step({"modbus_read": request})

    def test_fc16_step_validation(self):
        validate_step({"modbus_write_multiple": {"address": 4, "values": [600, 1], "expect_exception": 2}})
        for request in (
            {}, {"address": 4, "values": [600]},
            {"address": True, "values": [600], "expect_exception": 0},
            {"address": 65536, "values": [600], "expect_exception": 2},
            {"address": 4, "values": [], "expect_exception": 0},
            {"address": 4, "values": [True], "expect_exception": 0},
            {"address": 4, "values": [65536], "expect_exception": 2},
            {"address": 4, "values": [1] * 124, "expect_exception": 2},
            {"address": 4, "values": [600], "expect_exception": True},
        ):
            with self.subTest(request=request), self.assertRaises(ValueError):
                validate_step({"modbus_write_multiple": request})
        load_scenarios("scenarios/modbus_atomic_write.yaml")

    def test_modbus_scenario_checks_ready_and_fault(self):
        scenario, = load_scenarios("scenarios/modbus_read_state.yaml")
        reads = [step["modbus_read"] for step in scenario["steps"] if "modbus_read" in step]
        self.assertEqual([read["expect"] for read in reads], [[1, 0, 0, 500, 500], [3, 1, 0, 0, 500]])
        self.assertEqual(frame(bytes.fromhex("010300000005")), bytes.fromhex("01030000000585c9"))

    def test_wrong_modbus_values_fail_and_close_resources(self):
        scenario = {"name": "wrong_reply", "steps": [
            {"modbus_read": {"address": 0, "count": 1, "expect": [1]}}]}
        with patch("scripts.run_scenarios.Console") as console, patch("scripts.run_scenarios.Client") as client:
            console.return_value.transcript = []
            client.return_value.read.return_value = (3,)
            with self.assertRaisesRegex(AssertionError, "Modbus"):
                run_scenario(scenario, Path("unused.elf"), "qemu", [])
            client.return_value.socket.close.assert_called_once()
            console.return_value.close.assert_called_once()

    def test_modbus_response_crc_and_header_are_checked(self):
        payload = bytes.fromhex("0103020001")
        corrupt = bytearray(frame(payload))
        corrupt[-1] ^= 1
        responses = [bytes(corrupt), frame(bytes.fromhex("0203020001")),
                     frame(bytes.fromhex("0104020001")), frame(bytes.fromhex("0103040001"))]
        for response in responses:
            with self.subTest(response=response.hex()), patch("scripts.modbus_client.socket.socket") as socket:
                socket.return_value.recv.return_value = response
                client = Client("unused.sock", [])
                with self.assertRaises(AssertionError):
                    client.read(0, 1)
                client.socket.close()

    def test_repository_scenarios_are_valid(self):
        scenarios = load_scenarios("scenarios/controller.yaml")
        self.assertGreaterEqual(len(scenarios), 6)


if __name__ == "__main__":
    unittest.main()
