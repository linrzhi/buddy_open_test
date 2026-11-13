idf_component_register(SRCS "src/board.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_adc
                                     esp_driver_gpio)
