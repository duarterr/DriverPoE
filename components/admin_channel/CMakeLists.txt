idf_component_register(SRCS
                            "admin_channel.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES
                            devid
                            tps2378
                            hv9910
                            eth_init
                            voltage_sense
                            dmx_input
                            esp_netif
                            esp_timer
                            esp_hw_support
                            nvs_flash
                            mbedtls
                            esp_app_format
                            app_update)
