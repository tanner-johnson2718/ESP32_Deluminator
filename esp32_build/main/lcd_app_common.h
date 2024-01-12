#pragma once

#include "esp_wifi.h"

#define TIMER_DELAY_MS 1000

// LCD Common functions and data
wifi_promiscuous_filter_t pkt_sniffer_filt = 
{
    .filter_mask=WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
};

void lcd_scan_n_dump(void);