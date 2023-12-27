#pragma once

struct wifi_conf
{
    uint8_t ap_poll_prio;
    uint16_t ap_poll_delay_ms;
    uint8_t scan_list_size;
    uint8_t create_ap_if;
} typedef wifi_conf_t;

void init_wifi(wifi_conf_t* _conf);
void register_wifi(void);
void ui_add_wifi(void);