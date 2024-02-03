// Packet Sniffer. The base esp32 promicious wifi only allows for filtering on 
// packet type and some sub types and only gives a single stream of packets. 
// We add an extra layer of filtering of promiscious packets with this 
// component and the ability to multiplex the promiscious stream of pkts. We
// do more indepth packet identification in this module, and also do in place
// parsing. This gives down stream modules the ability subscrtibe to certain
// types and attributes found in a frame and have packets send to them pre
// indexed.
//
// Assumptions) We assume that this component has unfeddered and uninterrupted
//              access to the WHOLE wifi chip. No other tasks should be running
//              that use the wifi chip. Please kill this this component before
//              attempting to connect a client to the built in AP if the wifi
//              mode if STA/AP.
//
//              For now we only support Data and Management Type packets as 1
//              control packets clutter the inbound pkt queue and 2 most of the
//              interesting 802.11 behavor comes from data and mgmt packets.
//
// Resources)
//    * https://www.oreilly.com/library/view/80211-wireless-networks/0596100523/ch04.html
//    * https://en.wikipedia.org/wiki/802.11_frame_types

#pragma once
#include "dot11.h"
#include "driver/gptimer.h"

//*****************************************************************************
// We define a structure that allows one to specifiy which type and sub type
// of packets should invoke the call back attached to the filter. The bitmaps
// contain a 1 in the i-th bit if the packet is of type or subtype i where i is 
// the integer index of the type or subtype in the enums defined in dot11.h. If
// an inbound packet matches is of a type and subtype that is contained in the
// bitmap, then the associated cb is called.
//
// In the pkt_sniffer_cb the pkt is just a uint8_t* buffer and the meta data is
// a esp_wifi.h struct of  wifi_pkt_rx_ctrl_t*. We keep these void* to keep
// this interface as portable as possible.
//*****************************************************************************
typedef struct
{
    uint8_t type_bitmap;
    uint16_t mgmt_subtype_bitmap;
    uint16_t data_subtype_bitmap;
}  pkt_filter_t;

typedef void (*pkt_sniffer_cb_t)(void* pkt, 
                                 void* meta_data, 
                                 pkt_type_t type, 
                                 pkt_subtype_t subtype);

typedef struct 
{
    pkt_filter_t filter;
    pkt_sniffer_cb_t cb;
} pkt_sniffer_filtered_src_t;

typedef struct 
{
    uint64_t num_pkt_total;
    uint64_t num_data_pkt;
    uint64_t num_mgmt_pkt;
    uint64_t num_data_subtype[16];
    uint64_t num_mgmt_subtype[16];
    gptimer_handle_t timer;
} pkt_sniffer_stats_t;

pkt_sniffer_stats_t* pkt_sniffer_get_stats(void);

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
//                         call back.
//
// f) See above.
//
// Returns) ESP_OK if added, otherwise could be full or failed to grab lock
//*****************************************************************************
esp_err_t pkt_sniffer_add_filter(pkt_sniffer_filtered_src_t* f);


//****************************************************************************
// pkt_sniffer_add_type_subtype) Add the indicated type and subtype to the
//                               filter contained in the passed filtered src
//
// f - pointer to location that can fit a pkt_sniffer_filtered_src_t
// type - enum above. Checked if when viewed as a uint8_t is below 16
// subtype - enum above. Checked if when viewed as a uint8_t is below 16
//
// Returns) ESP_OK if all good, else invalid arg
//****************************************************************************
esp_err_t pkt_sniffer_add_type_subtype(pkt_sniffer_filtered_src_t* f, 
                                       pkt_type_t type, 
                                       pkt_subtype_t subtype);


//*****************************************************************************
// pkt_sniffer_launch) Given a specific channel and start the pkt_sniffer
//
// channel) Must be between 1 and 11
//
// type_filter) refer to esp_wifi documentation for details
//
// Returns) ESP OK If launched otherwise could have invalid arg or its already
//          running. Also could be failures to set the wifi chip to the
//          appropirate config.
//*****************************************************************************
esp_err_t pkt_sniffer_launch(uint8_t channel);


//*****************************************************************************
// pkt_sniffer_kill) Kills the already running sniffer.
//
// Return) ESP OK if killed. 
//*****************************************************************************
esp_err_t pkt_sniffer_kill(void);