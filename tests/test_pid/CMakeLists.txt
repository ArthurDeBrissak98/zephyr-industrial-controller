cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_pid)

target_include_directories(app PRIVATE ../../include)

# Подключаем файл с ПИД-регулятором для тестов
target_sources(app PRIVATE 
          src/main.c
    ../../src/control.c 
)