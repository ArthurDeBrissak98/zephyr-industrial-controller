"""Modbus RTU через UART1 настоящего QEMU, Shell отдельно на UART0."""
import argparse
from pathlib import Path
import socket
import struct
import tempfile
import time

from .demo_test import Console


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def frame(payload):
    return payload + struct.pack("<H", crc16(payload))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Client:
    def __init__(self, path, log):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(1)
        self.socket.connect(str(path))
        self.log = log

    def exchange(self, payload, size, corrupt=False):
        # Межкадровая пауза больше 3.5 символов при 19200 бод.
        time.sleep(0.02)
        request = bytearray(frame(payload))
        if corrupt:
            request[-1] ^= 1
        self.log.append("TX " + request.hex())
        self.socket.sendall(request)
        response = bytearray()
        try:
            while len(response) < size:
                chunk = self.socket.recv(size - len(response))
                require(chunk, "UART connection closed")
                response.extend(chunk)
        except socket.timeout:
            if size:
                raise AssertionError("Modbus response timed out: " + response.hex())
        self.log.append("RX " + response.hex())
        if size:
            require(crc16(response[:-2]) == int.from_bytes(response[-2:], "little"), "Bad response CRC")
        return bytes(response)

    def read(self, address=0, count=5):
        response = self.exchange(struct.pack(">BBHH", 1, 3, address, count), 5 + count * 2)
        require(response[:3] == bytes([1, 3, count * 2]), response.hex())
        return struct.unpack(">" + "H" * count, response[3:-2])

    def write(self, value):
        payload = struct.pack(">BBHH", 1, 6, 4, value & 0xFFFF)
        require(self.exchange(payload, 8) == frame(payload), "FC06 echo mismatch")

    def silent(self, payload, corrupt=False):
        time.sleep(0.02)
        request = bytearray(frame(payload))
        if corrupt:
            request[-1] ^= 1
        self.socket.sendall(request)
        self.socket.settimeout(0.2)
        try:
            require(not self.socket.recv(256), "Unexpected reply to ignored frame")
        except socket.timeout:
            pass
        finally:
            self.socket.settimeout(1)
        self.log.append("SILENT " + request.hex())


def run(elf, qemu, log):
    require(crc16(bytes.fromhex("01030000000a")) == 0xCDC5, "CRC implementation error")
    with tempfile.TemporaryDirectory(prefix="controller-modbus-") as directory:
        path = Path(directory, "rtu.sock")
        console = Console(elf, qemu, directory,
                          extra_args=["-serial", f"unix:{path},server=on,wait=off"])
        client = None
        try:
            console.status("READY")
            require("modbus_ready=yes" in console.command("modbus_status"), "RTU did not initialize")
            client = Client(path, log)
            require(client.read() == (1, 0, 0, 500, 500), "Wrong initial map")
            client.write(700)
            time.sleep(0.3)
            require(client.read(4, 1) == (700,), "Polling overwrote setpoint")
            require("reg4_setpoint_x10=700" in console.command("modbus_status"), "Shell map mismatch")
            for payload, exception in [
                (bytes.fromhex("010600000002"), bytes([1, 0x86, 2])),
                (bytes.fromhex("0106000403e9"), bytes([1, 0x86, 2])),
                (bytes.fromhex("010300040002"), bytes([1, 0x83, 2])),
                (bytes.fromhex("010400000001"), bytes([1, 0x84, 1])),
            ]:
                require(client.exchange(payload, 5)[:3] == exception, "Wrong exception response")
            require(client.read(4, 1) == (700,), "Rejected write changed setpoint")
            client.silent(bytes.fromhex("010600040258"), corrupt=True)
            client.silent(bytes.fromhex("020600040258"))
            require(client.read(4, 1) == (700,), "Ignored frame changed setpoint")
            client.silent(bytes.fromhex("00060004028a"))
            require(client.read(4, 1) == (650,), "Broadcast FC06 was not applied")
            client.write(700)
            require("OK" in console.command("start"), "Start failed")
            require(client.read(0, 1) == (2,), "RUNNING not visible")
            require("OK" in console.command("inject wire-break on"), "Injection failed")
            console.status("FAULT", active=1)
            require(client.read(0, 2) == (3, 1), "Fault not visible")
            client.write(600)
            require(client.read(0, 2) == (3, 1), "Write reset fault")
            require("OK" in console.command("inject wire-break off"), "Clear failed")
            require("OK" in console.command("reset"), "Reset failed")
            require("OK" in console.command("set profile ahu"), "Profile failed")
            console.status("READY")
            client.write(-50)
            time.sleep(0.3)
            require(client.read(4, 1) == (0xFFCE,), "Signed setpoint not preserved")
            require("reg4_setpoint_x10=-50" in console.command("modbus_status"), "Signed Shell value wrong")
            print("PASS: FC03/FC06, signed setpoint, exceptions, CRC, address, faults and separate Shell")
        finally:
            if client:
                client.socket.close()
            log.extend(console.transcript)
            console.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", type=Path, default=Path("build/zephyr/zephyr.elf"))
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--log", type=Path, default=Path("build/modbus-test.log"))
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
