idf_component_register(
    SRCS "mac_logger.c"
    INCLUDE_DIRS "."
    REQUIRES pkt_sniffer
    PRIV_REQUIRES esp_timer
)