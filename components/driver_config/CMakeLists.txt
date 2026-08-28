idf_component_register(SRCS
                            "driver_config.c"
                        INCLUDE_DIRS "include"
                        PRIV_REQUIRES
                            hv9910
                            nvs_flash)
