idf_component_register(SRCS
                            "poe_luminaire_main.c"
                            "hv9910.c"
                            "poe_negotiator.c"
                            "voltage_sense.c"
                            "eth_init.c"
                            "cmd_server.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES
                            esp_driver_gpio
                            esp_driver_ledc
                            esp_adc
                            esp_netif
                            esp_eth
                            esp_event
                            esp_timer
                            nvs_flash)
