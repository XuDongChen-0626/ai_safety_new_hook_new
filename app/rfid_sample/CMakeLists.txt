if(CONFIG_RFID_SAMPLE)
  target_sources(app PRIVATE
    driver/rfid_sample_uart.c
    driver/rfid_sample_protocol.c
    sample/rfid_sample.c
    shell/rfid_sample_shell.c
  )
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sample)
endif()
