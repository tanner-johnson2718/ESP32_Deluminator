idf_component_register(
    SRCS "eapol_logger.c"
    INCLUDE_DIRS "."
    REQUIRES pkt_sniffer
)