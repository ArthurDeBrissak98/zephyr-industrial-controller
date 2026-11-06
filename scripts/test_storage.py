"""Проверки отказов файловой flash на изолированных образах QEMU."""
import argparse
from pathlib import Path
import tempfile

from .demo_test import Console
from .run_scenarios import status_matches


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def test_unavailable(elf, qemu, log):
    with tempfile.TemporaryDirectory(prefix="controller-flash-error-") as directory:
        Path(directory, "controller_flash.bin").mkdir()
        console = Console(elf, qemu, directory)
        try:
            status = console.status("READY")
            require(status_matches(status, {"settings_ready": "no", "output_x100": 0}), status)
            require("Команда отклонена" in console.command("config save"), "Save accepted without flash")
            require("Команда отклонена" in console.command("motor set current 15000"), "Motor accepted without flash")
            motor = console.command("motor status")
            require("nominal_current_ma=11500" in motor and "type=induction" in motor, motor)
        finally:
            log.extend(console.transcript)
            console.close()
    print("PASS: недоступная flash оставляет дефолты и сообщает ошибку")


def test_corruption(elf, qemu, log):
    with tempfile.TemporaryDirectory(prefix="controller-flash-corrupt-") as directory:
        console = Console(elf, qemu, directory)
        try:
            require("OK" in console.command("set profile ahu"), "Profile change failed")
            require("OK" in console.command("set mode 0-10v"), "Mode change failed")
            require("OK" in console.command("config save"), "Save failed")
        finally:
            log.extend(console.transcript)
            console.close()
        path = Path(directory, "controller_flash.bin")
        image = bytearray(path.read_bytes())
        offset = image.find(b"1CIZ")
        require(offset >= 0, "Saved settings block not found")
        image[offset + 20] ^= 1
        path.write_bytes(image)
        console = Console(elf, qemu, directory)
        try:
            status = console.status("READY")
            require(status_matches(status, {"profile": "pump", "mode": "4-20ma",
                "settings_present": "no", "output_x100": 0}), status)
        finally:
            log.extend(console.transcript)
            console.close()
    print("PASS: повреждённая запись не применяется после перезапуска")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, default=Path("build/zephyr/zephyr.elf"))
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--log", type=Path, default=Path("build/storage-test.log"))
    args = parser.parse_args()
    log = []
    try:
        test_unavailable(args.elf.resolve(), args.qemu, log)
        test_corruption(args.elf.resolve(), args.qemu, log)
    finally:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text("\n".join(log))


if __name__ == "__main__":
    main()
