cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(controller_hw_io_tests)
target_include_directories(app PRIVATE ../../include)
target_compile_options(app PRIVATE -Wall -Wextra)
target_sources(app PRIVATE src/main.c ../../src/io_hw.c ../../src/io_scaling.c
    ../../src/controller.c ../../src/sensor_signal.c ../../src/protection.c)
