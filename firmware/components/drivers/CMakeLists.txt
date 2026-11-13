idf_component_register(SRCS "src/drv_led.c"
                            "src/drv_solenoid.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board
                                     esp_driver_gpio
                                     esp_driver_ledc
                                     esp_timer
                                     freertos
                                     log)
