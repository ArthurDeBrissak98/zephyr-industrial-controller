"""Подключение обработчиков целого FC03/FC16 к закреплённой версии Zephyr."""
import hashlib
from pathlib import Path
import sys

EXPECTED_SHA256 = "3a049c8e971032ba17574fd6d3c69635600ab84371f009fa8d7200697117e5dd"


def prepare(source, destination):
    data = Path(source).read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED_SHA256:
        raise ValueError("Версия modbus_server.c изменилась: проверьте адаптер FC03/FC16")
    text = data.decode()
    # Транспорт, CRC, адресация, broadcast и исключения остаются в Zephyr.
    for name, function in (("MODBUS_FC03_HOLDING_REG_RD", "mbs_fc03_hreg_read"),
                           ("MODBUS_FC16_HOLDING_REGS_WR", "mbs_fc16_hregs_write")):
        old = f"case {name}:\n\t\tsend_reply = {function}(ctx);"
        new = f"case {name}:\n\t\tsend_reply = mbs_try_user_fc(ctx, fc);"
        if text.count(old) != 1:
            raise ValueError(f"Не найден ожидаемый обработчик {name}")
        text = text.replace(old, new)
    # Эти штатные функции теперь не используются в нашей сборке.
    text = text.replace("static bool mbs_fc03_hreg_read(", "static bool __unused mbs_fc03_hreg_read(")
    text = text.replace("static bool mbs_fc16_hregs_write(", "static bool __unused mbs_fc16_hregs_write(")
    Path(destination).write_text(text)


if __name__ == "__main__":
    prepare(*sys.argv[1:])
