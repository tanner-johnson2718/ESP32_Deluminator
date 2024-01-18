// TODO
//   * Make it always filter on MGT and DATA

// Packet Sniffer. The base esp32 promicious cb system only allows for
// filtering on packet type and some sub types. We add an extra layer of 
// filtering of promiscious packets with this component. 
//
// Assumptions) We assume that this component has unfeddered and uninterrupted
//              access to the WHOLE wifi chip. No other tasks should be running
//              that use the wifi chip. Please kill this this component before
//              attempting to 
//
// Model) The execution model is as follows. We set the channel and type filter
//        These are component wide filters and the type filter is same as the 
//        wifi_promiscious_filter_t filter provided by esp promiscious module.
//        Now we provide a mechanism for adding a small number of filter sources.
//        So packets come in and are initially filtered by channel and by the
//        wifi_promiscious_filter_t filter. We register a queue to futher
//        filter packets. For each filter queue pair registered with the module,
//        we apply the passed filter and if it matches we push the pkt to the q. 
//        The params of the filter are shown in the 
//        pkt_sniffer_filtered_cb_t type.
//

#pragma once
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

//*****************************************************************************
// Filter Source. The following typedefs give structure to our further
// filtered packet sniffer. When launching the sniffer you have to set a 
// channel and packet type mask. We give the following type defs so you can 
// register a queue / filter pair. You can filter on the following params:
//
//    -> AP  MAC address
//    -> SRC MAC address
//    -> DST MAC address
//
// The filter is of positive nomenclature i.e. a pkt will be added to the q if 
// and only if it matches the params you supply. For each MAC address there is
// a "active" field that must be set in order for matching on that supplied MAC
// addr to take place.
//*****************************************************************************

struct pkt_sniffer_filtered_src
{
    uint8_t ap_active;
    uint8_t ap[6];
    uint8_t src_active;
    uint8_t src[6];
    uint8_t dst_active;
    uint8_t dst[6];
    QueueHandle_t q;
} typedef pkt_sniffer_filtered_src_t;

struct pkt_sniffer_msg
{
    wifi_promiscuous_pkt_t* p;
    wifi_promiscuous_pkt_type_t type;
} typedef pkt_sniffer_msg_t;

//*****************************************************************************
// Returns) 1 if running 0 else
//*****************************************************************************
uint8_t pkt_sniffer_is_running(void);

//*****************************************************************************
// pkt_sniffer_clear_filter_list) Clears all the filters, resets it back to 0
//
// Returns) ESP_OK, otherwise might timeout trying to grab the lock
//*****************************************************************************
esp_err_t pkt_sniffer_clear_filter_list(void);

//*****************************************************************************
// pkt_sniffer_add_filter) Fill in the filter src struct in accordance with the
//                         documentation provided above. Will add the filter to
//                         list of filters and if the sniffer is running, 
//                         matching packet will be sent your way via the passed
//                         q.
//
// f) See above.
//
// Returns) ESP_OK if added, otherwise could be full or failed to grab lock
//*****************************************************************************
esp_err_t pkt_sniffer_add_filter(pkt_sniffer_filtered_src_t* f);


//*****************************************************************************
// pkt_sniffer_launch) Given a specific channel and packet type filter, start
//                     pkt_sniffer
// channel) Must be between 1 and 11
//
// type_filter) refer to esp_wifi documentation for details
//
// Returns) ESP OK If launched otherwise could have invalid arg or its already
//          running. Also could be failures to set the wifi chip to the
//          appropirate config.
//*****************************************************************************
esp_err_t pkt_sniffer_launch(uint8_t channel, wifi_promiscuous_filter_t type_filter);

//*****************************************************************************
// pkt_sniffer_kill) Kills the already running sniffer.
//
// Return) ESP OK if killed. 
//*****************************************************************************
esp_err_t pkt_sniffer_kill(void);