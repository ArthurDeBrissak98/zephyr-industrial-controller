"""Клиент сырых RTU-кадров для UART QEMU через UNIX-сокет."""
import socket
import struct
import time


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

