idf_component_register(SRCS
                            "devid.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES
                            nvs_flash
                            esp_hw_support
                            mbedtls)
