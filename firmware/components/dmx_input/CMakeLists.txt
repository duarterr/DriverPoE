idf_component_register(SRCS
                            "dmx_input.c"
                            "dmx_artnet.c"
                            "dmx_sacn.c"
                        INCLUDE_DIRS "include"
                        PRIV_INCLUDE_DIRS "."
                        PRIV_REQUIRES
                            hv9910
                            tps2378
                            power_manager
                            devid
                            eth_init
                            esp_netif
                            esp_timer
                            lwip
                            nvs_flash)
