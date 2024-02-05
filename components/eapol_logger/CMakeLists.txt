idf_component_register(
    SRCS "eapol.c"
    INCLUDE_DIRS "."
    REQUIRES pkt_sniffer mac_logger
)

target_link_libraries(${COMPONENT_LIB} -Wl,-zmuldefs)