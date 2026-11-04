import tempfile
from pathlib import Path
import unittest

from scripts.run_scenarios import load_scenarios, status_matches, validate_step


class ScenarioValidationTests(unittest.TestCase):
    def test_bad_steps_are_rejected(self):
        steps = [
            {}, {"command": "start"}, {"command": "start\nstop", "expect": ["OK"]},
            {"command": "start", "expect": ["OK"], "restart": True},
            {"status": {}}, {"status": {"unknown": 1}},
            {"status": {"state": "READY"}, "timeout_ms": 0},
            {"wait_ms": -1}, {"wait_ms": True}, {"restart": False},
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

    def test_repository_scenarios_are_valid(self):
        scenarios = load_scenarios("scenarios/controller.yaml")
        self.assertGreaterEqual(len(scenarios), 6)


if __name__ == "__main__":
    unittest.main()
