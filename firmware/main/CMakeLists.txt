idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES board
                                     esp_pm
                                     esp_app_format
                                     drivers
                                     transport
                                     buddy_cmd)
