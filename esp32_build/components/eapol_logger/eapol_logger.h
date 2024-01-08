#pragma once
#include "pkt_sniffer.h"
#include "esp_err.h"
#include "esp_wifi.h"

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

// When flushing to disk we name the file the first 21 bytes or so of the
// ssid. This is just a caveat given that an ssid could be of 32 bytes but
// the max len of a file in spiffs is 32 including the mount point.

// Packet Doc)
//   * Assoc Req
//   * Assoc Res
//   * EAPOL / WPA2 handshake


//*****************************************************************************
// eapol_logger_init) Register the eapol_logger_cb with the pkt_sniffer and init
//                    the lock. note that if the pkt sniffer filters are cleared
//                    you will need to re add it with the provided call back and
//                    no mac filters and eapol only set on the filtered_cb 
//                    struct
//
// Returns) What ever pkt_sniffer_add_filter returns. The init of the semaphore
//          is asserted true.
//*****************************************************************************
esp_err_t eapol_logger_init(void);

//*****************************************************************************
// In the event one needs to re register this bad larry with the pkt sniffer
// we give it global linkage. This would only be required if one clears the
// pkt sniffer filters. Bes sure to include both MGMT and DATA packets when
// re adding to the filered cb list in the pkt sniffer.
//*****************************************************************************
void eapol_logger_cb(wifi_promiscuous_pkt_t* p, 
                     wifi_promiscuous_pkt_type_t type, 
                     WPA2_Handshake_Index_t eapol);

//*****************************************************************************
// REPL
//*****************************************************************************
int do_eapol_logger_init(int argc, char** argv);