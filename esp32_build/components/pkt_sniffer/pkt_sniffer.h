// Packet Sniffer. The base esp32 promicious cb system only allows for
// filtering on packet type and some sub types. We add an extra layer of 
// filtering with this component. 
//
// Assumptions) We assume that this component has unfeddered and uninterrupted
//              access to the WHOLE wifi chip. No other tasks should be running
//              that use the wifi chip.
//
// Model) The execution model is as follows. We set the channel and type filter
//        These are component wide  and the type filter is same as the 
//        wifi_promiscious_filter_t filter provided by esp promiscious module.
//        Now we provide a mechanism adding a small number of filter cb pairs.
//        So packets come in and are initially filtered by channel and by the
//        wifi_promiscious_filter_t filter. We register a call back to futher
//        filter packets. For each filter cb pair registered with the module,
//        we apply the passed filter and if it matches we call the associated 
//        cb. The params of the filter are shown in the 
//        pkt_sniffer_filtered_cb_t type.
//
// EAPOL Packet structure / theory ... TODO

struct pkt_sniffer_filtered_cb
{
    uint8_t ap_filter[6];
    uint8_t src_filter[6];
    uint8_t dst_filter[6];
    uint8_t eapol_only;
    // add cb here
} typedef pkt_sniffer_filtered_cb_t


uint8_t pkt_sniffer_is_running(void);

esp_err_t add_filtered_cb(pkt_sniffer_filtered_cb_t* f);

esp_err_t pkt_sniffer_launch(uint8_t channel, wifi_promiscuous_filter_t type_filter);

esp_err_t pkt_sniffer_kill(void);