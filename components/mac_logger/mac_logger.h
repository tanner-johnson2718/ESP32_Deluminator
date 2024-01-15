#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "pkt_sniffer.h"

// Goal of the component is to fill in this data structure by sniffing inbound
// packets:

// AP)
//    - SSID
//    - BSSID (MAC)
//    - Channel
//    - RSSI
//    - PMF Capable?
//    - authmode
//    - pairwise cipher
//    - group cipher
//    - eapol packet lens   (lens of each pkt in buffer)
//    - eapol packet buffer (fits 6 pkt of fixed max size)
//    - number of associated stas
//    - STA 0
//    - ...
//    - STA N-1

// We allocate a single buffer to store a single WPA2 handshake. When all 
// packets of the handshake have been found we flush them to disk and reset the
// in mem buffer that holds them. The in ram buffer is as follows:
//
// |-----------|-----------|---------|---------|---------|---------|
// | Assoc Req | Assoc Res | EAPOL 1 | EAPOL 2 | EAPOL 3 | EAPOL 4 |
// |-----------|-----------|---------|---------|---------|---------|
// 
// Each of the 6 packet slots gets a 256 buffer. If the packet is smaller we
// just pad the rest as we maintain the lengths of each packet. When dumping to
// disk and sending over network the lengths are written first, with each len
// fitting in a 2 byte integer, followed by the packet data with no padding.

// When flushing to disk we name the file the first 19 bytes or so of the
// ssid. This is just a caveat given that an ssid could be of 32 bytes but
// the max len of a file in spiffs is 32 including the mount point.


// Flow diagram)


//      TODO!!!!

// Notes on Beacon PKTs
//
// * Sent automatically by all acccess points at a regular interval.
// * FC = `0x8000`
//     * This decodes to a frame of type Management and a subtype of beacon
// * DA = `ff:ff:ff:ff:ff:ff` and SA=BSSID
// * In a beacon frame rate of beacon frame sent is advertised
// * Capabilites, SSID (the ascii name of the AP), Supported rates are all sent.
// * To see this, do a dump with the `-e` flag targeting an AP: `sudo airodump-ng wlp5s0mon -e --bssid B6:FE:F4:C3:ED:EE -c 6 -w out`
// * Open wire shark on the .cap file and you should see a butt load of beacons
// * When scanning for APs listening for beacons is considered a passive scan

// Note on Probe PKTs
// 
// * Deliberatley sent by the station (STA) to the AP aka an active AP scan
// * FC = `0x4000`
//     * This decodes to a managment type with probe request sub type
// * DA = `ff:ff:ff:ff:ff:ff` = BSSID
// * SA = <sender MAC>
// * The packet really only contains the STAs advertised rates and the ssid of a AP if the scan was targeted to an AP
// * To capture these packets we just set the ESP32 deluminator to scan our home AP and dumped packets targeting that AP

// The connection process of an STA connectiong to an ap involves a 3 stage 
// process. Authentication. Association and EAPOL. Authenitcation. This step is
// mainly kept from the days of WEP and can be relatively ignored except for 
// the fact that it initiates the next steps. Association. This step is 
// important because it is where the STA declares the ssid for which it is 
// connecting. If the AP is open this step would be the last. EAPOL. This is 
// where crypto key info is exchanged. This is what we need to capture in order 
// to capture ones encrypted psks.

#define SSID_MAX_LEN 33
#define MAC_LEN 6
#define EAPOL_MAX_PKT_LEN 256
#define EAPOL_NUM_PKTS    6

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
    uint8_t eapol_written_out;
    uint8_t eapol_pkt_lens[EAPOL_NUM_PKTS];
    uint8_t eapol_buffer[EAPOL_MAX_PKT_LEN * EAPOL_NUM_PKTS];
    uint8_t num_assoc_stas;
    sta_t stas[CONFIG_MAC_LOGGER_MAX_STAS];
} typedef ap_t;

struct ap_summary
{
    uint8_t ssid[SSID_MAX_LEN];
    uint8_t bssid[MAC_LEN];
    uint8_t channel;
    int8_t rssi;
    uint8_t eapol_written_out;
    uint8_t num_assoc_stas;
} typedef ap_summary_t;

//*****************************************************************************
// mac_logger_init) Register the mac_logger_cb with the pkt_sniffer and init
//                  the lock. If one clears the pkt filters, which deletes
//                  the mac logger call back, then this can be recalled to
//                  re-register it. WARNING, only recall this init func if
//                  youve cleared the pkt sniffer funcs.
//
// ap_mac) Optional AP mac filter. If not NULL, the mac logger will only add
//         stas that are sending traffic whose AP mac addr field matches that
//         that is passed.
//
// Returns) What ever pkt_sniffer_add_filter returns. The init of the semaphore
//          is asserted true.
//*****************************************************************************
esp_err_t mac_logger_init(uint8_t* ap_mac);


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
esp_err_t mac_logger_get_ap(uint8_t ap_index, ap_summary_t* ap);


//*****************************************************************************
// mac_logger_get_sta) copy the sta from the indicated indicies into the passed
//                     sta buffer.
//
// ap_index) index of ap to get sta from. Checked if in range.
//
// sta_index) sta index within ap. Checked if in range.
//
// Returns) OK            - Index valid, lock grabbed, sta filled
//          INAVLID STATE - Cant grab Lock
//          INVALID_ARG   - Index out of range
//*****************************************************************************
esp_err_t mac_logger_get_sta(uint8_t ap_index, uint8_t sta_index, sta_t* sta);


//*****************************************************************************
// mac_logger_clear) reset both the sta and ap list
//
// Return) OK            - Successfully cleared the lists
//         INVALID_STATE - couldn't get lock
//*****************************************************************************
esp_err_t mac_logger_clear(void);