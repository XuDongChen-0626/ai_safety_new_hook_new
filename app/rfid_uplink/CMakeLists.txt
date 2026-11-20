target_sources_ifdef(CONFIG_RFID_UPLINK app PRIVATE
	rfid_uplink.c
	rfid_uplink_protocol.c
)

if(CONFIG_RFID_UPLINK)
	target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
