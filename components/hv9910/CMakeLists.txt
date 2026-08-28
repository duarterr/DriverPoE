idf_component_register(SRCS
                            "hv9910.c"
                            "hv9910_curve.c"
                        INCLUDE_DIRS "."
                        REQUIRES
                            esp_driver_gpio
                        PRIV_REQUIRES
                            esp_driver_ledc)
