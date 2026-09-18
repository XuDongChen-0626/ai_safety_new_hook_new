add_library(offline_log_service STATIC)
add_dependencies(offline_log_service zephyr_generated_headers)

target_include_directories(offline_log_service PUBLIC .)
target_link_libraries(offline_log_service PUBLIC
	zephyr_interface
	time_manager_service
	device_identity_service
	fault_dump_service
)

target_sources_ifdef(CONFIG_OFFLINE_LOG_SERVICE
	offline_log_service
	PRIVATE
	offline_log_service.c
	offline_log_sd.c
)

# Shell and mcumgr registration objects must be compiled into the app target
# directly: inside a static library they only contain section-registered
# entries and can be dropped by the linker.
target_sources_ifdef(CONFIG_OFFLINE_LOG_SHELL app PRIVATE
	offline_log_shell.c
)

target_sources_ifdef(CONFIG_OFFLINE_LOG_MCUMGR app PRIVATE
	offline_log_mgmt.c
)
