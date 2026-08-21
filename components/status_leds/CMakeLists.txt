idf_component_register(SRCS
                            "status_leds.c"
                        INCLUDE_DIRS "."
                        REQUIRES
                            esp_driver_gpio)
