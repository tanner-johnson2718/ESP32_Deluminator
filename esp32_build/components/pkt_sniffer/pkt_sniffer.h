// Packet Sniffer. The base esp32 promicious cb system only allows for
// filtering on packet type and some sub types. We add an extra layer of 
// filtering with this component. 
//
// Assumptions) We assume that this component has unfeddered and uninterrupted
//              access to the WHOLE wifi chip. No other tasks should be running
//              that use the wifi chip.
//
// Model) The execution model is as follows. We set the channel and type filter
//        These are component wide filters and the type filter is same as the 
//        wifi_promiscious_filter_t filter provided by esp promiscious module.
//        Now we provide a mechanism adding a small number of filter cb pairs.
//        So packets come in and are initially filtered by channel and by the
//        wifi_promiscious_filter_t filter. We register a call back to futher
//        filter packets. For each filter cb pair registered with the module,
//        we apply the passed filter and if it matches we call the associated 
//        cb. The params of the filter are shown in the 
//        pkt_sniffer_filtered_cb_t type.
//

#include "esp_wifi.h"

//*****************************************************************************
// WPA2 Handshake index. One of the main tasks of this component is to capture
// and identify WPA2 handshake packets.
//*****************************************************************************
enum WPA2_Handshake_Index
{
    WPA2_HS_ASSOC_REQ,
    WPA2_HS_ASSOC_RES,
    EAPOL_HS_1,
    EAPOL_HS_2,
    EAPOL_HS_3,
    EAPOL_HS_4,
    HS_NONE             // Identifies the lack of relavanet WPA2 Hand shake pkt
} typedef WPA2_Handshake_Index_t;

//*****************************************************************************
// Filter CB Pairs. The following typedefs give structure to our further
// filtered packet sniffer. When launching the sniffer you have to set a 
// channel and packet type mask. We give the following type defs so you can 
// register a callback / filter pair. You can filter on the following params:
//
//    -> AP  MAC address
//    -> SRC MAC address
//    -> DST MAC address
//    -> is EAPOL or of interest in a WPA2 handshake
//
// The filter is of positive nomenclature i.e. your callback will only be 
// called if it matches the params you supply. For each MAC address there is an
// "active" field that must be set in order for matching on that supplied MAC
// addr to take place.
//*****************************************************************************
typedef void (*pkt_sniffer_cb_t)(wifi_promiscuous_pkt_t* p, 
                                 wifi_promiscuous_pkt_type_t type, 
                                 WPA2_Handshake_Index_t eapol);

struct pkt_sniffer_filtered_cb
{
    uint8_t eapol_only;
    uint8_t ap_active;
    uint8_t ap[6];
    uint8_t src_active;
    uint8_t src[6];
    uint8_t dst_active;
    uint8_t dst[6];
    pkt_sniffer_cb_t cb;
} typedef pkt_sniffer_filtered_cb_t;


uint8_t pkt_sniffer_is_running(void);

esp_err_t pkt_sniffer_clear_filter_list(void);

esp_err_t pkt_sniffer_add_filter(pkt_sniffer_filtered_cb_t* f);

esp_err_t pkt_sniffer_launch(uint8_t channel, wifi_promiscuous_filter_t type_filter);

esp_err_t pkt_sniffer_kill(void);

//*****************************************************************************
// REPL Test Driver Functions
//*****************************************************************************

int do_pkt_sniffer_add_filter(int argc, char** argv);
int do_pkt_sniffer_launch(int argc, char** argv);
int do_pkt_sniffer_kill(int argc, char** argv);
int do_pkt_sniffer_clear(int argc, char** argv);