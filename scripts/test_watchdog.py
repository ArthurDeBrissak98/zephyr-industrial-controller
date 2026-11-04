"""Проверка аппаратного сброса MPS2 без перезапуска процесса QEMU."""
import argparse
import os
from pathlib import Path
import time

from .demo_test import Console
from .run_scenarios import status_matches

BOOT = "Watchdog armed: timeout_ms=2000 reset=SOC"


def collect(console, duration, until_boot=False):
    output = bytearray()
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        if console.selector.select(0.05):
            chunk = os.read(console.process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError("QEMU завершился вместо аппаратного сброса")
            output.extend(chunk)
            if until_boot and BOOT in output.decode(errors="replace"):
                break
    text = output.decode(errors="replace")
    console.transcript.append(text)
    return text


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(elf, qemu, log):
    console = Console(elf, qemu, machine="mps2-an385")
    try:
        console.status("READY")
        require("OK" in console.command("set profile ahu"), "Profile change failed")
        require("OK" in console.command("set mode 0-10v"), "Mode change failed")
        require("OK" in console.command("config save"), "Save failed")
        collect(console, 0.2)
        # Если кормление отсутствует или идёт не из цикла, проверка ниже это обнаружит.
        require(BOOT not in collect(console, 4.5), "Watchdog reset during healthy polling")
        for attempt in range(2):
            require("OK" in console.command("start"), "Start failed")
            time.sleep(0.3)
            require("event=START" in console.command("events"), "Start event missing")
            console.status("RUNNING")
            process = console.process.pid
            started = time.monotonic()
            response = console.command("crash_test")
            require("OK" in response, "Crash test rejected")
            # Shell жив, завис именно основной поток. Его состояние пока сохраняется.
            time.sleep(0.2)
            console.status("RUNNING")
            output = collect(console, 5, until_boot=True)
            elapsed = time.monotonic() - started
            require(BOOT in output, "No boot after polling thread hung")
            require(1.3 <= elapsed <= 3.5, f"Unexpected watchdog timeout: {elapsed:.2f}s")
            require(console.process.pid == process and console.process.poll() is None,
                    "QEMU was replaced instead of resetting the emulated SoC")
            # Сообщение main приходит раньше готовности потока Shell.
            if "uart:~$ " not in output:
                console.read_prompt()
            status = console.status("READY")
            require(status_matches(status, {"profile": "ahu", "mode": "0-10v",
                    "settings_present": "yes", "output_x100": 0, "active": 0}), status)
            require("event=START" not in console.command("events"), "Journal did not reset")
            print(f"PASS: hardware reset {attempt + 1} after {elapsed:.2f}s; settings restored, output off")
    finally:
        log.extend(console.transcript)
        console.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, default=Path("build-watchdog/zephyr/zephyr.elf"))
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--log", type=Path, default=Path("build-watchdog/watchdog-test.log"))
    args = parser.parse_args()
    log = []
    try:
        run(args.elf.resolve(), args.qemu, log)
    except (OSError, RuntimeError, TimeoutError, AssertionError) as error:
        log.append(f"FAIL: {error}")
        raise
    finally:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text("\n".join(log))


if __name__ == "__main__":
    main()
