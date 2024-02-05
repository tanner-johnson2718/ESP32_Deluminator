// Goal of the MAC logger is to get an idea of what APs are close and how many
// STAs are currently active on the AP. We have a data structure per AP and
// embeeded in the AP data structure is a static list of STA macs. We collect
// meta data on the AP as well. This component only looks at 4 types of packets
//
// 1) Probe Response -> Create AP
// 2) Beacon -> Create AP
// 3) Data -> based on DS flag we can idenitify that an STA is sending or 
//            recving traffic on the AP
// 4) QoS Data -> same as a data

#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "pkt_sniffer.h"

#define SSID_MAX_LEN 33
#define MAC_LEN 6

struct sta
{
    uint8_t mac[MAC_LEN];     // MAC Addr
    int8_t rssi;              // Last Known Signal Strength
} typedef sta_t;

struct ap
{
    uint8_t ssid[SSID_MAX_LEN];
    uint8_t bssid[MAC_LEN];
    uint8_t channel;
    int8_t rssi;
    uint32_t group_cipher_suite;
    uint32_t pairwise_cipher_suite;
    uint32_t auth_key_management;
    uint8_t num_assoc_stas;
    sta_t stas[CONFIG_MAC_LOGGER_MAX_STAS];
} typedef ap_t;


//*****************************************************************************
// mac_logger_launch) Create the component wide lock if it hasnt already then
//                    add the filter and call back to the pkt sniffer. Can be
//                    recalled if pkt sniffer filter list is cleared.
//
// Returns) OK            - Everything good
//          INVALID_STATE - Already running or task create fail
//          NO_MEM        - Q create fail
//*****************************************************************************
esp_err_t mac_logger_init(void);


//*****************************************************************************
// mac_logger_ap_list_len) Put the current ap list in the passed pointer
//
// *n) Must be valid address as is not checked. 0 if there is timeout waiting
//     for the lock else it is the current sta list size.
//
// Returns) ESP OK if actaul list len is used, else INVALID STATE if failed to
//          grab the lock
//*****************************************************************************
esp_err_t mac_logger_get_ap_list_len(uint8_t* n);


//*****************************************************************************
// mac_logger_get_ap) Copies from the indicated index to the passed struct the
//                    elements of an ap_summary_t.
//
// ap_list_index) index to copy from, is checked for range
//
// ap) Allocated chunk memory that can hold an ap_summary_t.
//
// Returns) OK            - Index valid, lock grabbed, sta filled
//          INAVLID STATE - Cant grab Lock
//          INVALID_ARG   - Index out of range
//*****************************************************************************
esp_err_t mac_logger_get_ap(uint8_t ap_index, ap_t* ap);

//*****************************************************************************
// mac_logger_clear) Clear the AP list
//
// Return) OK            - Successfully cleared the lists
//         INVALID_STATE - couldn't get lock
//*****************************************************************************
esp_err_t mac_logger_clear(void);