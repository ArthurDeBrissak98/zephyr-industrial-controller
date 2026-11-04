# Демонстрационный контроллер на Zephyr и QEMU

Проект для демо: эмулированный аналоговый датчик, управление приводом и программная защита от обрыва 4–20 мА. Основная платформа — `qemu_cortex_m3` (ARM Cortex-M3, TI LM3S6965). Реальные ADC и частотный преобразователь пока не подключены.

## Что проверяем

`Пуск → инъекция обрыва → останов → фиксация аварии → запись в журнал → устранение обрыва → сброс → отдельный пуск`.

После загрузки выход равен нулю. Первый отсчёт переводит контроллер из `STOPPED` в `READY`. Только команда `start` переводит его в `RUNNING`. ПИ-регулятор ограничивает команду диапазоном 0–50 Гц и меняет её не более чем на 1 Гц за цикл.

При токе ниже 3,6 мА контроллер переходит в `FAULT` и сразу задаёт выходу ноль, без плавного торможения. Исчезновение обрыва не снимает зафиксированную аварию. `reset` разрешён только после восстановления сигнала; он возвращает состояние `READY`, но не запускает привод. `stop` тоже не сбрасывает аварию.

Период опроса — 100 мс. Системный тест проверяет реакцию не позднее 150 мс после инъекции с учётом планирования потоков. Это проверка программы в QEMU, а не гарантия времени реакции будущей платы.

Профили `pump` и `ahu` задают разные шкалы измерения и уставки. Вход можно переключить между `4-20ma` и `0-10v`, пока контроллер остановлен и нет зафиксированной аварии. Инъекция обрыва доступна только для токовой петли. Самопроизвольных аварий и автоматического сброса больше нет.

## Подготовка

Проверенная ревизия Zephyr: `6848730b4b3f4d6e9151a0a8873955878be1a18d`. Нужны Linux, Python 3.12+, CMake 3.28+, Ninja, DTC, ARM GNU Toolchain и QEMU ARM. Zephyr SDK для этого варианта не требуется.

Пример для Ubuntu 24.04:

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends git python3-venv python3-dev \
  cmake ninja-build gperf device-tree-compiler make gcc-arm-none-eabi qemu-system-arm
python3 -m venv ../zephyr-venv
. ../zephyr-venv/bin/activate
pip install west
git init ../zephyr-workspace/zephyr
git -C ../zephyr-workspace/zephyr remote add origin https://github.com/zephyrproject-rtos/zephyr
git -C ../zephyr-workspace/zephyr fetch --depth 1 origin 6848730b4b3f4d6e9151a0a8873955878be1a18d
git -C ../zephyr-workspace/zephyr checkout --detach FETCH_HEAD
west init -l ../zephyr-workspace/zephyr
cd ../zephyr-workspace
west update cmsis cmsis_6
pip install -r zephyr/scripts/requirements-base.txt \
  -r zephyr/scripts/requirements-build-test.txt \
  -r zephyr/scripts/requirements-run-test.txt
export ZEPHYR_BASE="$PWD/zephyr"
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=/usr
cd ../zephyr-industrial-controller
```

В следующих терминалах снова активируйте venv и задайте три переменные выше с правильным путём к Zephyr.

## Сборка и запуск

```bash
python3 build.py -c -r
```

По умолчанию скрипт собирает `qemu_cortex_m3`. Эквивалентные команды:

```bash
west build -b qemu_cortex_m3 -d build .
west build -d build -t run
```

Для выхода из QEMU: `Ctrl+A`, затем `X`.

## Демонстрация через Shell

Дождитесь `uart:~$`, затем выполняйте команды:

```text
status
start
status
inject wire-break on
status
events
reset
inject wire-break off
status
reset
status
start
```

После `inject wire-break on` подождите один цикл опроса. Ожидается `state=FAULT`, `active=0x1`, `latched=0x1`, `current_ua=2000`, `output_x100=0`. Первый `reset` должен быть отклонён, поскольку обрыв ещё активен. После `inject wire-break off` дождитесь `active=0x0`: состояние останется `FAULT`, а защёлка — `0x1`. Только следующий `reset` вернёт `READY` с нулевым выходом.

Другие команды:

```text
stop
set profile ahu
set mode 0-10v
status
```

`events` выводит последние 32 события в порядке возникновения: номер, время от загрузки, тип события, состояние, маску аварий, ток и команду частоты. `FAULT` с `faults=0x1` означает обрыв токовой петли. При заполнении журнала старые записи вытесняются. Журнал хранится в RAM и не переживает перезапуск.

## Автоматические проверки

```bash
python "$ZEPHYR_BASE/scripts/twister" -T tests -p qemu_cortex_m3 -O twister-out -j 4
python scripts/demo_test.py --elf build/zephyr/zephyr.elf
```

Первый запуск проверяет логику состояний, порог защиты, плавный пуск, журнал и полный сценарий с настоящим периодическим потоком и эмулированным выходом. Второй запускает само приложение в QEMU и выполняет демонстрацию через UART, включая проверку текста журнала и запрета повторного пуска. Протокол сохраняется в `build/demo-test.log`.

GitHub Actions собирает приложение, запускает оба набора Ztest в QEMU и проверку Shell. Результаты тестов и протокол сохраняются как артефакт.

## Перенос на плату

- `controller.c` — автомат состояний, ПИ-регулятор, защита и журнал; не зависит от Zephyr и конкретных устройств.
- `controller_app.c` — периодический опрос и синхронизация доступа из Shell.
- `controller_io.h` — граница между логикой и устройствами.
- `io_sim.c` — текущая модель датчиков и выхода привода.

Для платы нужна своя реализация `controller_io.h`: чтение ADC, преобразование в физические величины и передача команды приводу. Она подключается в CMake вместо `io_sim.c`. Отключение `CONFIG_APP_SIMULATED_IO` сейчас намеренно останавливает сборку, пока аппаратный backend не добавлен. Функция инъекции на реальном backend должна возвращать `-ENOTSUP`.

Modbus-заглушки и старые демонстрационные защиты удалены: сетевого управления, аппаратной STO и испытаний на реальном оборудовании в этой версии нет. Следующий этап — выбрать плату и проверить драйверы, масштабирование ADC, отказ входов и задержки на реальных сигналах.
