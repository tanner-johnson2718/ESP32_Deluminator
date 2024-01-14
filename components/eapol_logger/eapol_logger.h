#pragma once
#include "pkt_sniffer.h"
#include "esp_err.h"
#include "esp_wifi.h"

// The EAPOL loggers purpose is to capture the handshake of an STA connecting
// to an AP. The connection process involves a 3 stage process. Authentication
// association and EAPOL.

// Authenitcation. This step is mainly kept from the days of WEP and can be 
// relatively ignored except for the fact that it initiates the next steps.

// Association. This step is important because it is where the STA declares
// the ssid for which it is connecting. If the AP is open this step would
// be the last.

// EAPOL. This is where crypto key info is exchanged. This is what we need to
// capture in order to capture ones encrypted psks.

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


//*****************************************************************************
// eapol_logger_init) Register the eapol_logger_cb with the pkt_sniffer and init
//                    the lock. This init function can be recalled to re ad the
//                    eapol filter back to the list of filters in pkt logger.
//
// Returns) What ever pkt_sniffer_add_filter returns. The init of the semaphore
//          is asserted true.
//*****************************************************************************
esp_err_t eapol_logger_init(uint8_t* ap_mac);