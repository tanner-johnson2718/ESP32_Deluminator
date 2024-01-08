#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "pkt_sniffer.h"

// In this component we implement our own AP and STA logger. An AP is a wifi
// acces point. An STA is a device or station that may or may be associated
// with an AP. This component requires the pkt sniffer component. 

// Data Structures)
//
//   STA List                     AP List
// |------------------------|     |---------|
// | S0 | S1 | S2 | S3 | S4 |     | A0 | A1 |
// |------------------------|     |---------|   
//         |         |               |   |
//         |         --------------------|
//         --------------------------|
//
// Consider the above example. We have seen a total of 5 unique MACs on the
// channel we are scanning. We send out a probe request and hear a response or 
// we possibly intercept a beacon telling us the MAC we stored in the 1th pos
// is actually an AP. We then further parse that packet to fill in the details
// in the AP struct and add it to the AP list at the 0th pos. Thus the AP list
// is like an addendum to some of the MACS stored in the STA list. Their are
// indexes in each struct pointing back to one another and if a MAC is not an
// AP then we set that index to -1.


#define SSID_MAX_LEN 33
#define MAC_LEN 6

struct sta
{
    uint8_t mac[MAC_LEN];     // MAC Addr
    int8_t rssi;              // Last Known Signal Strength
    int8_t ap_list_index;     // -1 if not AP
} typedef sta_t;

struct ap
{
    uint8_t ssid[SSID_MAX_LEN];
    uint8_t channel;
    int16_t sta_list_index;
} typedef ap_t;

// Packet Structures) 
//    * Beacon
//    * Probe Request
//    * Probe Response

//*****************************************************************************
// mac_logger_init) Register the mac_logger_cb with the pkt_sniffer and init
//                  the lock. note that if the pkt sniffer filters are cleared
//                  you will need to re add it with the provided call back and
//                  no mac filters set on the filtered_cb struct.
//
// Returns) What ever pkt_sniffer_add_filter returns. The init of the semaphore
//          is asserted true.
//*****************************************************************************
esp_err_t mac_logger_init(void);

//*****************************************************************************
// mac_logger_sta_list_len) Put the current sta list in the passed pointer
//
// *n) Must be valid address as is not checked. 0 if there is timeout waiting
//     for the lock else it is the current sta list size.
//
// Returns) ESP OK if actaul list len is used, else INVALID STATE if failed to
//          grab the lock
//***************************************************************************** 
esp_err_t mac_logger_get_sta_list_len(int16_t* n);

//*****************************************************************************
// mac_logger_ap_list_len) Put the current ap list in the passed pointer
//
// *n) Must be valid address as is not checked. 0 if there is timeout waiting
//     for the lock else it is the current sta list size.
//
// Returns) ESP OK if actaul list len is used, else INVALID STATE if failed to
//          grab the lock
//*****************************************************************************
esp_err_t mac_logger_get_ap_list_len(int8_t* n);

//*****************************************************************************
// mac_logger_get_sta) Copies the sta struct from the sta list at the given
//                     index to passed memory
//
// sta_list_index) index to copy from, is checked for range
//
// sta) A allocated chunk of memory that can fit a sta_t. Is not checked.
//
// Returns) OK            - Index valid, lock grabbed, sta filled
//          INAVLID STATE - Cant grab Lock
//          INVALID_ARG   - Index out of range
//*****************************************************************************
esp_err_t mac_logger_get_sta(int16_t sta_list_index, sta_t* sta);

//*****************************************************************************
// mac_logger_get_sta) Copies the sta struct and the ap struct from a given
//                     ap index into the passed memory. Note this index is for
//                     the seperate AP list, not the overall list of STAs. This
//                     is useful if one wants to list all APs.
//
// ap_list_index) index to copy from, is checked for range
//
// sta) A allocated chunk of memory that can fit a sta_t. Is not checked.
//
// ap) Allocated chunk memory that can hold an ap_t.
//
// Returns) OK            - Index valid, lock grabbed, sta filled
//          INAVLID STATE - Cant grab Lock
//          INVALID_ARG   - Index out of range
//          NOT_FOUND     - STA list index in AP struct is out of range. This
//                          is a rather bad error and indicates corruption.
//*****************************************************************************
esp_err_t mac_logger_get_ap(int8_t ap_list_index, sta_t* sta, ap_t* ap);

//*****************************************************************************
// In the even on needs to re register this bad larry with the pkt sniffer
// we give it global linkage.
//*****************************************************************************
void mac_logger_cb(wifi_promiscuous_pkt_t* p, 
                   wifi_promiscuous_pkt_type_t type, 
                   WPA2_Handshake_Index_t eapol);


//*****************************************************************************
// REPL Test functions
//*****************************************************************************
int do_mac_logger_start_dump(int argc, char** argv);
int do_mac_logger_stop_dump(int argc, char** argv);
int do_mac_logger_init(int argc, char** argv);
