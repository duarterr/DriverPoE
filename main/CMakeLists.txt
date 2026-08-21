idf_component_register(SRCS
                            "poe_luminaire_main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES
                            hv9910
                            tps2378
                            voltage_sense
                            eth_init
                            devid
                            admin_channel
                            status_leds
                            esp_timer
                            app_update)
