idf_component_register(SRCS
                            "tps2378.c"
                        INCLUDE_DIRS "."
                        REQUIRES
                            esp_driver_gpio
                            voltage_sense
                        PRIV_REQUIRES
                            esp_system)
