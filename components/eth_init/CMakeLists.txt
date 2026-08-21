idf_component_register(SRCS
                            "eth_init.c"
                        INCLUDE_DIRS "."
                        REQUIRES
                            esp_netif
                            esp_driver_gpio
                        PRIV_REQUIRES
                            esp_eth
                            esp_event)
