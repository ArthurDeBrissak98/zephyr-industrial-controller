# Zephyr этой версии имеет только callback отдельного регистра для FC03/FC16.
# Собираем копию сервера с маршрутизацией этих двух функций через user FC API.
# Исходники SDK не меняются; несовпадение версии останавливает конфигурацию.
set(modbus_server_source ${ZEPHYR_BASE}/subsys/modbus/modbus_server.c)
set(modbus_server_copy ${CMAKE_CURRENT_BINARY_DIR}/controller_modbus_server.c)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${modbus_server_source} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_modbus_server.py)
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_modbus_server.py
        ${modbus_server_source} ${modbus_server_copy}
    COMMAND_ERROR_IS_FATAL ANY)
get_target_property(modbus_sources subsys__modbus SOURCES)
list(FILTER modbus_sources EXCLUDE REGEX "(^|/)modbus_server\\.c$")
set_property(TARGET subsys__modbus PROPERTY SOURCES ${modbus_sources})
target_sources(subsys__modbus PRIVATE ${modbus_server_copy})
