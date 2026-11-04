cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(controller_modbus_tests)
target_include_directories(app PRIVATE ../../include)
target_compile_options(app PRIVATE -Wall -Wextra)
target_sources(app PRIVATE src/main.c ../../src/controller.c ../../src/sensor_signal.c
    ../../src/protection.c ../../src/controller_app.c ../../src/io_sim.c ../../src/controller_modbus.c)
