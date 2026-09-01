idf_component_register(SRCS
                            "poe_luminaire_main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES
                            hv9910
                            driver_config
                            tps2378
                            power_manager
                            voltage_sense
                            eth_init
                            devid
                            admin_channel
                            dmx_input
                            status_leds
                            app_update)
