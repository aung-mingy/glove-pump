idf_component_register(SRCS "glove_pump.c"
                    # Declared explicitly: main inherits every component only
                    # while it declares nothing — the moment it lists one, the
                    # rest stop resolving and the includes break.
                    PRIV_REQUIRES esp_driver_gpio
                                  esp_driver_usb_serial_jtag
                                  esp_adc
                    INCLUDE_DIRS ".")
