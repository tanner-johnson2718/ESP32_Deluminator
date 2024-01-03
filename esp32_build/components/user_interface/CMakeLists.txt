idf_component_register(
    SRCS "encoder.c" "user_interface.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES HD44780 esp_timer
    REQUIRES driver)