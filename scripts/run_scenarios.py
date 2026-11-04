"""YAML-сценарии для консоли контроллера в QEMU."""
import argparse
from pathlib import Path
import re
import sys
import tempfile
import time
import zlib

import yaml
from .demo_test import Console
from .modbus_client import Client

STATUS_FIELDS = {
    "sensor_fault_names", "process_fault_names", "read_error", "sensor_faults", "process_faults", "state", "profile", "mode", "active", "latched", "warnings", "output_x100",
    "frequency_x100", "current_ua", "voltage_mv", "scaled_x10", "measured_x10",
    "signal_valid", "settings_ready", "settings_present", "settings_error",
    "pid_bias_x100", "pid_kp", "pid_ki", "pid_divisor", "active_names", "latched_names",
    "warning_names", "signal_injected", "wire_break_injected",
}


class ScenarioLoader(yaml.SafeLoader):
    def construct_mapping(self, node, deep=False):
        mapping = super().construct_mapping(node, deep=deep)
        if len(mapping) != len(node.value):
            raise ValueError("Повторяющийся ключ YAML")
        return mapping


def load_scenarios(path):
    data = yaml.load(Path(path).read_text(), Loader=ScenarioLoader)
    if (not isinstance(data, dict) or set(data) != {"version", "scenarios"}
            or type(data["version"]) is not int or data["version"] != 1):
        raise ValueError("Нужны version: 1 и список scenarios")
    scenarios = data["scenarios"]
    if not isinstance(scenarios, list) or not scenarios:
        raise ValueError("Список scenarios пуст")
    names = set()
    for scenario in scenarios:
        if not isinstance(scenario, dict) or set(scenario) != {"name", "steps"}:
            raise ValueError("У сценария должны быть name и steps")
        name = scenario["name"]
        if not isinstance(name, str) or not name.strip() or name in names:
            raise ValueError("Имена сценариев должны быть непустыми и уникальными")
        names.add(name)
        steps = scenario["steps"]
        if not isinstance(steps, list) or not steps:
            raise ValueError(f"{name}: нет шагов")
        for step in steps:
            validate_step(step)
    return scenarios


def validate_step(step):
    if not isinstance(step, dict):
        raise ValueError("Шаг должен быть словарём")
    actions = set(step) & {"command", "status", "wait_ms", "restart", "corrupt_settings_crc", "modbus_read"}
    if len(actions) != 1:
        raise ValueError("В шаге должно быть ровно одно действие")
    action = next(iter(actions))
    allowed = {"command", "expect"} if action == "command" else (
        {"status", "timeout_ms"} if action == "status" else {action})
    if set(step) - allowed:
        raise ValueError("Неизвестное поле шага")
    if action == "command":
        command, expected = step["command"], step.get("expect")
        if not isinstance(command, str) or not command.strip() or "\n" in command or "\r" in command:
            raise ValueError("Некорректная команда")
        if len(command.encode()) >= 128:
            raise ValueError("Команда длиннее буфера Shell")
        if (not isinstance(expected, list) or not expected
                or any(not isinstance(x, str) or not x for x in expected)):
            raise ValueError("Для команды нужен непустой список expect")
    elif action == "status":
        expected = step["status"]
        if not isinstance(expected, dict) or not expected or set(expected) - STATUS_FIELDS:
            raise ValueError("Некорректные поля status")
        if any(type(x) not in (str, int) for x in expected.values()):
            raise ValueError("Значения status должны быть строками или числами; yes/no заключайте в кавычки")
        timeout = step.get("timeout_ms", 2000)
        if type(timeout) is not int or not 1 <= timeout <= 10000:
            raise ValueError("timeout_ms должен быть от 1 до 10000")
    elif action == "modbus_read":
        request = step[action]
        if not isinstance(request, dict) or set(request) != {"address", "count", "expect"}:
            raise ValueError("modbus_read требует address, count и expect")
        address, count, expected = request["address"], request["count"], request["expect"]
        if (type(address) is not int or type(count) is not int
                or not 0 <= address < 5 or not 1 <= count <= 5 or address + count > 5):
            raise ValueError("modbus_read: запрос выходит за карту регистров 0–4")
        if (not isinstance(expected, list) or len(expected) != count
                or any(type(x) is not int or not 0 <= x <= 65535 for x in expected)):
            raise ValueError("modbus_read: expect должен содержать count чисел uint16")
    elif action == "wait_ms":
        if type(step["wait_ms"]) is not int or not 0 <= step["wait_ms"] <= 5000:
            raise ValueError("wait_ms должен быть от 0 до 5000")
    elif step[action] is not True:
        raise ValueError(f"{action} должен быть true")


def status_matches(output, expected):
    fields = dict(re.findall(r"(\w+)=([^\s]+)", output))
    for key, wanted in expected.items():
        actual = fields.get(key)
        if actual is None:
            return False
        if type(wanted) is int:
            try:
                if int(actual, 0) != wanted:
                    return False
            except ValueError:
                return False
        elif actual != wanted:
            return False
    return True


def corrupt_settings_crc(storage):
    """Портим CRC приложения, сохраняя корректную контрольную сумму NVS."""
    path = Path(storage, "controller_flash.bin")
    image = bytearray(path.read_bytes())
    marker = b"1CIZ" + (1).to_bytes(4, "little")
    offset = image.find(marker)
    if offset < 0 or image.find(marker, offset + 1) >= 0 or offset + 104 > len(image):
        raise ValueError("Нужна ровно одна запись настроек версии 1")
    blob = image[offset:offset + 100]
    stored_crc = int.from_bytes(blob[96:100], "little")
    if zlib.crc32(blob[:96]) != stored_crc:
        raise ValueError("CRC настроек уже повреждена")
    if zlib.crc32(blob) != int.from_bytes(image[offset + 100:offset + 104], "little"):
        raise ValueError("Некорректная CRC записи NVS")
    image[offset + 96] ^= 1
    image[offset + 100:offset + 104] = zlib.crc32(image[offset:offset + 100]).to_bytes(4, "little")
    path.write_bytes(image)


def run_scenario(scenario, elf, qemu, log):
    console = None
    modbus = None
    uses_modbus = any("modbus_read" in step for step in scenario["steps"])
    log.append(f"SCENARIO {scenario['name']}")
    with tempfile.TemporaryDirectory(prefix="controller-scenario-") as storage:
        socket_path = Path(storage, "modbus.sock")

        def start_instance():
            extra = []
            if uses_modbus:
                socket_path.unlink(missing_ok=True)
                extra = ["-serial", f"unix:{socket_path},server=on,wait=off"]
            instance = Console(elf, qemu, storage, extra_args=extra)
            try:
                return instance, Client(socket_path, log) if uses_modbus else None
            except Exception:
                instance.close()
                raise

        try:
            console, modbus = start_instance()
            for index, step in enumerate(scenario["steps"], 1):
                log.append(f"STEP {index}: {step}")
                if "command" in step:
                    output = console.command(step["command"])
                    for expected in step["expect"]:
                        if expected not in output:
                            raise AssertionError(f"Нет ожидаемого текста {expected!r}: {output}")
                elif "status" in step:
                    deadline = time.monotonic() + step.get("timeout_ms", 2000) / 1000
                    while True:
                        output = console.command("status")
                        if status_matches(output, step["status"]):
                            break
                        if time.monotonic() >= deadline:
                            raise AssertionError(f"Не дождались status {step['status']}: {output}")
                        time.sleep(0.02)
                elif "modbus_read" in step:
                    request = step["modbus_read"]
                    actual = modbus.read(request["address"], request["count"])
                    if actual != tuple(request["expect"]):
                        raise AssertionError(f"Modbus: ожидали {request['expect']}, получили {list(actual)}")
                elif "wait_ms" in step:
                    time.sleep(step["wait_ms"] / 1000)
                else:
                    if modbus:
                        modbus.socket.close()
                        modbus = None
                    log.extend(console.transcript)
                    console.close()
                    console = None
                    if "corrupt_settings_crc" in step:
                        corrupt_settings_crc(storage)
                    console, modbus = start_instance()
            print(f"PASS: {scenario['name']}")
        finally:
            if modbus:
                modbus.socket.close()
            if console:
                log.extend(console.transcript)
                console.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", type=Path, default=Path("scenarios/controller.yaml"))
    parser.add_argument("--elf", type=Path, default=Path("build/zephyr/zephyr.elf"))
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--log", type=Path, default=Path("build/scenarios.log"))
    parser.add_argument("--scenario", action="append")
    args = parser.parse_args()
    log = []
    try:
        scenarios = load_scenarios(args.file)
        if args.scenario:
            unknown = set(args.scenario) - {x["name"] for x in scenarios}
            if unknown:
                raise ValueError(f"Неизвестные сценарии: {unknown}")
            scenarios = [x for x in scenarios if x["name"] in args.scenario]
        for scenario in scenarios:
            run_scenario(scenario, args.elf.resolve(), args.qemu, log)
    except (ValueError, OSError, AssertionError, RuntimeError, TimeoutError, yaml.YAMLError) as error:
        log.append(f"FAIL: {error}")
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text("\n".join(log))
    return 0


if __name__ == "__main__":
    sys.exit(main())
