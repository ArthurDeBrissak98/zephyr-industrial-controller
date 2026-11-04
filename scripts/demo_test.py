#!/usr/bin/env python3
"""Проверка демонстрации через UART настоящего процесса QEMU."""
import argparse
import os
from pathlib import Path
import re
import selectors
import subprocess
import time
import tempfile


class Console:
    def __init__(self, elf, qemu, storage_dir=None, machine="lm3s6965evb", extra_args=None):
        self.storage = tempfile.TemporaryDirectory(prefix="controller-qemu-") if storage_dir is None else None
        workdir = self.storage.name if self.storage else str(storage_dir)
        elf = Path(elf).resolve()
        if os.path.dirname(qemu):
            qemu = str(Path(qemu).resolve())
        self.process = subprocess.Popen(
            [qemu, "-machine", machine, "-cpu", "cortex-m3",
             "-display", "none", "-monitor", "none", "-serial", "stdio",
             "-nic", "none", "-semihosting-config", "enable=on,target=native", "-kernel", str(elf)] + (extra_args or []),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=workdir,
        )
        self.closed = False
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.transcript = []
        try:
            self.read_prompt()
        except Exception:
            self.close()
            raise

    def read_prompt(self, command=None):
        output = b""
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.selector.select(0.1):
                chunk = os.read(self.process.stdout.fileno(), 65536)
                if not chunk:
                    raise RuntimeError("QEMU завершился до ответа консоли")
                output += chunk
                text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", output.decode(errors="replace"))
                echo = text.find(command) if command else 0
                if echo >= 0 and text.rfind("uart:~$ ") > echo:
                    self.transcript.append(text)
                    return text
        raise TimeoutError("Нет приглашения Shell: " + output.decode(errors="replace"))

    def command(self, text):
        self.process.stdin.write((text + "\n").encode())
        self.process.stdin.flush()
        return self.read_prompt(text)

    def status(self, state, active=None):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            output = self.command("status")
            if f"state={state}" in output and (active is None or f"active=0x{active:x}" in output):
                return output
            time.sleep(0.02)
        raise AssertionError(f"Не дождались состояния {state}: {output}")

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.selector.close()
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        if self.storage:
            self.storage.cleanup()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, default=Path("build/zephyr/zephyr.elf"))
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--log", type=Path, default=Path("build/demo-test.log"))
    args = parser.parse_args()
    console = Console(args.elf, args.qemu)
    try:
        assert "Команда отклонена" in console.command("crash_test")
        initial = console.status("READY", active=0)
        assert "active_names=none" in initial and "latched_names=none" in initial
        assert "signal_injected=no" in initial and "wire_break_injected=no" in initial
        assert "OK" in console.command("start")
        time.sleep(0.3)
        running = console.status("RUNNING")
        assert int(re.search(r"output_x100=(\d+)", running)[1]) > 0
        assert "OK" in console.command("inject wire-break on")
        fault = console.status("FAULT", active=1)
        assert "latched=0x1" in fault and "output_x100=0" in fault
        assert "active_names=WIRE_BREAK" in fault and "latched_names=WIRE_BREAK" in fault
        assert "wire_break_injected=yes" in fault
        assert "Команда отклонена" in console.command("reset")
        journal = console.command("events")
        assert re.search(r"t=\d+ms event=FAULT state=FAULT faults=0x1 current_ua=2000 frequency_x100=0", journal)
        assert "OK" in console.command("inject wire-break off")
        fault = console.status("FAULT", active=0)
        assert "latched=0x1" in fault and "output_x100=0" in fault
        assert "Команда отклонена" in console.command("start")
        assert "OK" in console.command("reset")
        time.sleep(0.2)
        ready = console.status("READY", active=0)
        assert "latched=0x0" in ready and "output_x100=0" in ready
        assert "OK" in console.command("start")
        console.status("RUNNING")
        assert "OK" in console.command("stop")
        console.status("READY")
        assert "OK" in console.command("set profile ahu")
        assert "profile=ahu" in console.status("READY")
        assert "OK" in console.command("set mode 0-10v")
        console.status("READY")
        assert "Команда отклонена" in console.command("inject wire-break on")
        injected = console.command("inject signal 5000")
        assert "OK" in injected and "override=5000 unit=mV" in injected
        time.sleep(0.5)
        voltage = console.status("READY", active=0)
        assert "scaled_x10=100 measured_x10=100 signal_valid=yes" in voltage
        assert "signal_injected=yes injected_value=5000 injected_unit=mV" in voltage
        assert "filter_samples=4/4 measurement_unit=celsius_x10" in voltage
        assert "Команда отклонена" in console.command("inject signal 12oops")
        assert "Команда отклонена" in console.command("inject signal 99999999999999")
        assert "injected_value=5000" in console.status("READY")
        assert "OK" in console.command("start")
        assert "OK" in console.command("inject signal 11000")
        fault = console.status("FAULT", active=2)
        assert "latched=0x2" in fault and "output_x100=0" in fault
        assert "signal_valid=no" in fault
        assert "active_names=INPUT_RANGE" in fault
        assert "filter_samples=0/4" in fault
        assert "Команда отклонена" in console.command("reset")
        journal = console.command("events")
        assert re.search(r"event=FAULT state=FAULT faults=0x2.*voltage_mv=11000", journal)
        restored = console.command("inject signal off")
        assert "OK" in restored and "signal source=model" in restored
        assert "OK" in console.command("reset")
        console.status("READY", active=0)
        assert "OK" in console.command("set profile pump")
        assert "OK" in console.command("set mode 4-20ma")
        assert "low-warning" in console.command("protect show")
        assert "Команда отклонена" in console.command("protect set low-trip 400 -1 500")
        assert "Команда отклонена" in console.command("protect set low-trip 400 20 60001")
        assert "Команда отклонена" in console.command("protect set unknown 400 20 500")
        assert "OK" in console.command("protect disable high-warning")
        assert "high-warning enabled=no" in console.command("protect show")
        assert "OK" in console.command("protect set low-warning 450 20 300")
        assert "OK" in console.command("protect set low-trip 400 20 1200")
        assert "Команда отклонена" in console.command("protect set low-trip 950 0 100")
        assert "Команда отклонена" in console.command("protect set high-trip 400 0 100")
        assert "Команда отклонена" in console.command("protect set low-warning 900 0 100")
        assert "Команда отклонена" in console.command("protect set high-warning 450 0 100")
        console.status("READY")
        assert "OK" in console.command("start")
        assert "Команда отклонена" in console.command("protect set low-trip 400 20 500")
        assert "OK" in console.command("inject signal 10400")
        time.sleep(0.85)
        warning = console.status("RUNNING", active=0)
        assert "warnings=0x1" in warning
        assert "active_names=none" in warning and "warning_names=LOW" in warning
        assert int(re.search(r"output_x100=(\d+)", warning)[1]) > 0
        journal = console.command("events")
        assert re.search(r"event=WARNING .*warnings=0x1", journal)
        fault = console.status("FAULT", active=4)
        assert "latched=0x4" in fault and "output_x100=0" in fault
        assert "active_names=PROCESS_LOW" in fault
        assert "signal_valid=yes" in fault
        assert "Команда отклонена" in console.command("reset")
        # 43% снимают аварию, но ещё не снимают предупреждение с границей 47%.
        assert "OK" in console.command("inject signal 10880")
        recovered = console.status("FAULT", active=0)
        assert "warnings=0x1" in recovered
        assert "OK" in console.command("reset")
        assert "output_x100=0" in console.status("READY")
        assert "OK" in console.command("start")
        console.status("RUNNING")
        assert "OK" in console.command("inject signal 12000")
        time.sleep(0.5)
        assert "warnings=0x0" in console.status("RUNNING", active=0)
        assert re.search(r"event=WARNING_CLEARED .*warnings=0x1", console.command("events"))
        journal = console.command("events")
        assert "profile=ahu mode=0-10v signal_valid=no fault_names=INPUT_RANGE" in journal
        assert "profile=pump mode=4-20ma signal_valid=yes fault_names=PROCESS_LOW" in journal
        assert "OK" in console.command("stop")
        assert "OK" in console.command("protect set high-warning 550 20 0")
        assert "OK" in console.command("protect set high-trip 600 50 0")
        assert "OK" in console.command("inject signal 20000")
        fault = console.status("FAULT", active=8)
        assert "active_names=PROCESS_HIGH" in fault and "warning_names=HIGH" in fault
        assert "OK" in console.command("inject wire-break on")
        combined = console.status("FAULT", active=9)
        assert "active_names=WIRE_BREAK,PROCESS_HIGH" in combined
        assert "latched_names=WIRE_BREAK,PROCESS_HIGH" in combined
        assert "OK" in console.command("inject signal off")
        assert "OK" in console.command("inject wire-break off")
        assert "OK" in console.command("reset")
        console.status("READY", active=0)
        print("PASS: единицы инъекции, расшифровка причин и снимок настроек в журнале")
        print("PASS: предупреждение без останова, задержка аварии, гистерезис и ручной сброс")
        print("PASS: пуск → обрыв → останов → журнал → устранение → сброс → отдельный пуск")
        print("PASS: масштабирование AHU, усреднение, ошибки команды и выход напряжения за диапазон")
    finally:
        console.close()
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text("\n".join(console.transcript))


if __name__ == "__main__":
    main()
