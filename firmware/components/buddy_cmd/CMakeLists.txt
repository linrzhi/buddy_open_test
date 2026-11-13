idf_component_register(SRCS "src/buddy_cmd.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board
                                     drivers
                                     esp_app_format
                                     esp_system
                                     esp_timer
                                     freertos
                                     log)
