cmake_minimum_required(VERSION 3.20.0)

# The custom board definition lives inside this repository.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(craner_ai_safety_hook)

# HKCRC reusable Zephyr services (kept as git submodules so that every
# project can be updated independently).
add_subdirectory(module/system_health_service)
add_subdirectory(module/time_manager_service)
add_subdirectory(module/ota_manager_service)
add_subdirectory(module/fault_dump_service)

# Hook product applications.
add_subdirectory(app/hook_modbus)
add_subdirectory(app/hook_alarm)
add_subdirectory(app/hook_bms)
add_subdirectory(app/hook_relay)
add_subdirectory(app/hook_gps)
add_subdirectory(app/hook_4g)
add_subdirectory(app/hook_supervisor)
add_subdirectory(app/hook_health)
add_subdirectory(app/hook_ota)

target_include_directories(app PRIVATE
	src
	component/uart_manage
	component/am_modbus
	component/nmea
	app/hook_modbus
	app/hook_alarm
	app/hook_bms
	app/hook_relay
	app/hook_gps
	app/hook_4g
	app/hook_supervisor
	app/hook_health
	app/hook_ota
)

target_sources(app PRIVATE
	src/main.c
	src/board.c
	src/hook_task.c
	src/hook_shell.c
	src/modbus_rtu_master.c
	src/modbus_rtu_slave.c
	src/uart_manage_port.c
	src/osal_zephyr.c
	src/pwr_stm32.c
	component/uart_manage/uart_manage.c
	component/uart_manage/fifo.c
	component/uart_manage/lwrb.c
	component/am_modbus/Modbus.c
	component/nmea/nmea.c
)

target_link_libraries(app PRIVATE
	system_health_service
	time_manager_service
	ota_manager_service
	fault_dump_service
)
