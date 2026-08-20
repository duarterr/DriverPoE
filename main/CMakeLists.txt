idf_component_register(SRCS
                            "poe_luminaire_main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES
                            poe_luminaire
                            esp_driver_gpio)
