import unittest
from unittest.mock import Mock, patch

from scripts.demo_test import Console


def status_reply(active, names):
    return (f"status\r\nstate=FAULT profile=pump mode=4-20ma\r\n"
            f"active=0x{active:x} latched=0x9 current_ua=2000\r\n"
            f"active_names={names}\r\nwarning_names=HIGH\r\nuart:~$ ")


class ConsoleStatusTests(unittest.TestCase):
    def test_log_mask_does_not_replace_status_mask(self):
        console = Console.__new__(Console)
        old = status_reply(8, "PROCESS_HIGH")
        old += "\n[00:00:04.000] <err> controller_app: FAULT active=0x9\n"
        console.command = Mock(side_effect=[old, status_reply(9, "WIRE_BREAK,PROCESS_HIGH")])
        result = console.status("FAULT", active=9)
        self.assertIn("active_names=WIRE_BREAK,PROCESS_HIGH", result)
        self.assertEqual(console.command.call_count, 2)

    def test_fault_mask_is_compared_as_complete_value(self):
        console = Console.__new__(Console)
        console.command = Mock(side_effect=[status_reply(0x10, "INPUT_IO"), status_reply(1, "WIRE_BREAK")])
        result = console.status("FAULT", active=1)
        self.assertIn("active_names=WIRE_BREAK", result)
        self.assertEqual(console.command.call_count, 2)

    def test_redrawn_prompt_does_not_cut_command_reply(self):
        console = Console.__new__(Console)
        console.process = Mock()
        console.transcript = []
        console.selector = Mock()
        console.selector.select.side_effect = [[True], [True], []]
        chunks = [b"status\r\nstate=FAULT\r\nuart:~$ ",
                  b"\r\nactive_names=WIRE_BREAK,PROCESS_HIGH\r\nwarning_names=HIGH\r\nuart:~$ "]
        with patch("scripts.demo_test.os.read", side_effect=chunks) as read:
            output = console.read_prompt("status")
        self.assertIn("active_names=WIRE_BREAK,PROCESS_HIGH", output)
        self.assertEqual(read.call_count, 2)

    def test_split_ansi_sequence_after_prompt_is_consumed(self):
        console = Console.__new__(Console)
        console.process = Mock()
        console.transcript = []
        console.selector = Mock()
        console.selector.select.side_effect = [[True], [True], []]
        with patch("scripts.demo_test.os.read", side_effect=[b"status\r\nuart:~$ \x1b[", b"m"]):
            output = console.read_prompt("status")
        self.assertEqual(output, "status\r\nuart:~$ ")

    def test_partial_status_is_not_accepted(self):
        console = Console.__new__(Console)
        console.command = Mock(side_effect=["status\nstate=FAULT\nactive=0x9\nuart:~$ ",
                                            status_reply(9, "WIRE_BREAK,PROCESS_HIGH")])
        result = console.status("FAULT", active=9)
        self.assertIn("active_names=WIRE_BREAK,PROCESS_HIGH", result)
        self.assertEqual(console.command.call_count, 2)


if __name__ == "__main__":
    unittest.main()
