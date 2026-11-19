cmake_minimum_required(VERSION 3.20)
project(rfid_protocol_tests C)

enable_testing()

add_executable(rfid_protocol_tests
	test_rfid_protocol.c
	../../app/rfid_sample/driver/rfid_sample_protocol.c
)
target_include_directories(rfid_protocol_tests PRIVATE
	../../app/rfid_sample/driver
)
target_compile_features(rfid_protocol_tests PRIVATE c_std_11)
set_target_properties(rfid_protocol_tests PROPERTIES C_EXTENSIONS OFF)

if(MSVC)
	target_compile_options(rfid_protocol_tests PRIVATE /W4 /WX)
else()
	target_compile_options(rfid_protocol_tests PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()

add_test(NAME rfid_protocol COMMAND rfid_protocol_tests)
