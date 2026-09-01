idf_component_register(SRCS
                            "power_manager.c"
                        INCLUDE_DIRS "."
                        REQUIRES
                            tps2378
                        PRIV_REQUIRES
                            hv9910
                            driver_config
                            esp_timer)
