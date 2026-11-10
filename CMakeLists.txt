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
add_subdirectory(module/modbus_data_model)
add_subdirectory(module/device_param_server)
add_subdirectory(module/device_identity_service)
add_subdirectory(component/offline_log_service)

# Hook product applications.
add_subdirectory(app/hook_modbus)
add_subdirectory(app/hook_alarm)
add_subdirectory(app/hook_bms)
add_subdirectory(app/hook_heartbeat)
add_subdirectory(app/hook_rtc)
add_subdirectory(app/hook_4g)
add_subdirectory(app/hook_4g_cfg)
add_subdirectory(app/hook_4g_ntp)
add_subdirectory(app/hook_work_mode)
add_subdirectory(app/hook_health)
add_subdirectory(app/hook_ota)
add_subdirectory(app/hook_params)
add_subdirectory(app/log_manage_app)

target_include_directories(app PRIVATE
	src
	app/hook_modbus
	app/hook_alarm
	app/hook_bms
	app/hook_heartbeat
	app/hook_rtc
	app/hook_4g
	app/hook_4g_cfg
	app/hook_4g_ntp
	app/hook_work_mode
	app/hook_health
	app/hook_ota
	app/hook_params
)

target_sources(app PRIVATE
	src/main.c
	src/board.c
	src/hook_task.c
	src/hook_shell.c
	src/hook_system_state.c
	src/hook_4g_uart.c
	src/hook_console.c
	src/modbus_rtu_master.c
	src/modbus_rtu_slave.c
	src/pwr_stm32.c
)

target_link_libraries(app PRIVATE
	system_health_service
	time_manager_service
	ota_manager_service
	fault_dump_service
	modbus_data_model
	device_param_server
	device_identity_service
	offline_log_service
)
