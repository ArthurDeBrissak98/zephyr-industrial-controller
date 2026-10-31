cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(system_monitor)

target_include_directories(app PRIVATE include)

target_sources(app PRIVATE
    src/main.c
    src/sensor.c
    src/control.c
    src/monitor.c
    src/modbus_client.c
    src/cli.c
)