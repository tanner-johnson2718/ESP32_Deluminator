idf_component_register(
    SRCS  "user_interface.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES HD44780 encoder
    REQUIRES driver)