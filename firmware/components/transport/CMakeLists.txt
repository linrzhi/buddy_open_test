idf_component_register(SRCS "src/usb_serial_port.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_usb_serial_jtag
                                     freertos
                                     log)
