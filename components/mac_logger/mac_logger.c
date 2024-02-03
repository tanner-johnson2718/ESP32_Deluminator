#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "mac_logger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "dot11_mgmt.h"

static SemaphoreHandle_t lock;
static ap_t ap_list[CONFIG_MAC_LOGGER_MAX_APS] = { 0 };
static uint8_t ap_list_len = 0;
static uint8_t one_time_init_done = 0;

static const char* TAG = "MAC LOGGER";

#define MOUNT_PATH "/spiffs"

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

    if(!xSemaphoreTake(lock, CONFIG_MAC_LOGGER_WAIT_MS / portTICK_PERIOD_MS))
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
// Private AP list accessors
//*****************************************************************************

static inline uint8_t mac_is_eq(uint8_t* m1, uint8_t* m2)
{
    uint8_t i = 0;
    for(; i < MAC_LEN; ++i)
    {
        if(m1[i]!=m2[i])
        {
            return 0;
        }
    }

    return 1;
}

static inline void clear_ap(uint8_t i)
{
    if(i >= CONFIG_MAC_LOGGER_MAX_APS)
    {
        ESP_LOGE(TAG, "Tried to clear out of range AP");
        return;
    }

    memset(ap_list + i, 0, sizeof(ap_t));
}

static inline void clear_ap_list(void)
{
    uint8_t i;
    for(i = 0; i < CONFIG_MAC_LOGGER_MAX_APS; ++i)
    {
        clear_ap(i);
    }

    ap_list_len = 0;
}

static inline void find_ap(uint8_t* ap_mac, int8_t* index)
{
    uint8_t i;
    for(i = 0; i < ap_list_len; ++i)
    {       
        if(mac_is_eq(ap_mac, ap_list[i].bssid))
        {
            *index = i;
            return;
        }
    }

    *index = -1;
}

static inline void find_sta(uint8_t* sta_mac, int8_t ap_index, int8_t* sta_index)
{
    if(ap_index >= ap_list_len)
    {
        ESP_LOGE(TAG, "Queried STA w/ invalid AP index");
        return;
    }

    uint8_t j;
    for(j = 0; j < ap_list[ap_index].num_assoc_stas; ++j)
    {
        if(mac_is_eq(sta_mac, ap_list[ap_index].stas[j].mac))
        {
            *sta_index = j;
            return;
        }
    }


    *sta_index = -1;
}

static inline void update_ap_rssi(uint8_t ap_index, int8_t rssi)
{
    if(ap_index >= ap_list_len)
    {
        ESP_LOGE(TAG, "Tried to update rssi of AP with invalid AP index");
        return;
    }
    ap_list[ap_index].rssi = rssi;
}

static inline void update_sta_rssi(uint8_t ap_index, uint8_t sta_index, int8_t rssi)
{
    if(ap_index >= ap_list_len)
    {
        ESP_LOGE(TAG, "Tried to update rssi of STA with invalid AP index");
        return;
    }

    if(sta_index >= ap_list[ap_index].num_assoc_stas)
    {
        ESP_LOGE(TAG, "Tried to update rssi of STA with invalid STA index");
        return;
    }

    ap_list[ap_index].stas[sta_index].rssi = rssi;
}

static inline void create_ap(char* ssid, uint8_t ssid_len, uint8_t* bssid, uint8_t channel, int8_t rssi)
{
    if(ap_list_len >= CONFIG_MAC_LOGGER_MAX_APS)
    {
        ESP_LOGE(TAG, "AP List Full");
        return;
    }

    memcpy(ap_list[ap_list_len].ssid, ssid, ssid_len);
    ap_list[ap_list_len].ssid[ssid_len] = 0;
    memcpy(ap_list[ap_list_len].bssid, bssid, MAC_LEN);
    ap_list[ap_list_len].channel = channel;
    ap_list[ap_list_len].rssi = rssi;
    ap_list_len++;

    ESP_LOGI(TAG, "AP Added: %s (%d/%d)", ap_list[ap_list_len-1].ssid, ap_list_len, CONFIG_MAC_LOGGER_MAX_APS);
}

static inline void create_sta(uint8_t ap_index, uint8_t* sta_mac, int8_t rssi)
{
    if(ap_index >= ap_list_len )
    {
        ESP_LOGE(TAG, "Tried to create sta with invalid AP index");
        return;
    }

    if(ap_list[ap_index].num_assoc_stas >= CONFIG_MAC_LOGGER_MAX_STAS)
    {
        ESP_LOGE(TAG, "AP %s sta list full", ap_list[ap_index].ssid);
        return;
    }

    memcpy(ap_list[ap_index].stas[ap_list[ap_index].num_assoc_stas].mac, sta_mac, MAC_LEN);
    ap_list[ap_index].stas[ap_list[ap_index].num_assoc_stas].rssi = rssi;
    ap_list[ap_index].num_assoc_stas++;

    ESP_LOGI(TAG, "STA "MACSTR" added to %s (%d/%d)", MAC2STR(sta_mac), ap_list[ap_index].ssid, ap_list[ap_index].num_assoc_stas, CONFIG_MAC_LOGGER_MAX_STAS);
}

//*****************************************************************************
// Parse Inbound Packets
//*****************************************************************************

void parse_beacon_pkt(beacon_t* hdr, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    if(_take_lock()){return;}

    int8_t index;
    find_ap(hdr->mgmt_header.addr3, &index);

    if(hdr->tagged_params[0] != TAGGED_PARAM_SSID)
    {
        ESP_LOGE(TAG, "SSID in beacon pkt not found");
        _release_lock();
        return;
    }

    uint8_t ssid_len = hdr->tagged_params[1];
    char* ssid = (char*) (hdr->tagged_params + 2);

    if(ssid_len == 0)
    {
        _release_lock();
        return;
    }

    if(index < 0)
    { 
        // search for the DS_param set in the tagged params to give the active
        // channel of the AP
        uint8_t* end_ptr = ((uint8_t*)hdr) + (rx_ctrl->sig_len - 4);
        uint8_t* start_ptr = hdr->tagged_params + 2 + ssid_len;
        uint8_t active_channel = 255;
        while(start_ptr < end_ptr)
        {
            if(start_ptr[0] == TAGGED_PARAM_DS_PARAM)
            {
                active_channel = *(start_ptr+2);
                break;
            }

            start_ptr += start_ptr[1] + 2;
        }

        if(active_channel == 255)
        {
            ESP_LOGE(TAG, "Couldnt find active channel in beacon / probe res");
            _release_lock();
            return;
        }

        create_ap(ssid, ssid_len, hdr->mgmt_header.addr3, active_channel, rx_ctrl->rssi);
    }
    else
    {
        update_ap_rssi(index, rx_ctrl->rssi);
    }

    _release_lock();
    return;
}

void parse_data_pkt(dot11_header_t* hdr, wifi_pkt_rx_ctrl_t* rx_ctrl)
{

    if(_take_lock()){ return; }

    uint8_t* ap_mac;
    uint8_t* sta_mac;
    int8_t ap_index;
    int8_t sta_index;

    if(hdr->ds_status == 1)   // toDS
    {
        ap_mac = hdr->addr1;
        sta_mac = hdr->addr2;
    }
    else if(hdr->ds_status == 2)  // fromDS
    {
        sta_mac = hdr->addr3;
        ap_mac = hdr->addr2;
    }
    else
    {
        ESP_LOGE(TAG, "Data pkt skipped");
        _release_lock();
        return;
    }

    //esp_log_write(ESP_LOG_INFO, "", "%d\n", hdr->ds_status);
    //esp_log_write(ESP_LOG_INFO, "", MACSTR"\n", MAC2STR(hdr->addr1));
    //esp_log_write(ESP_LOG_INFO, "", MACSTR"\n", MAC2STR(hdr->addr2));
    //esp_log_write(ESP_LOG_INFO, "", MACSTR"\n\n", MAC2STR(hdr->addr3));

    
    find_ap(ap_mac, &ap_index);
    
    if(ap_index < 0)
    {
        _release_lock();
        return;
    }

    find_sta(sta_mac, ap_index, &sta_index);
    if(sta_index < 0) { create_sta(ap_index, sta_mac, rx_ctrl->rssi );       }
    else {              update_sta_rssi(ap_index, sta_index, rx_ctrl->rssi); }

    _release_lock();
    return;
}

void mac_logger_cb(void* pkt, 
                   void* meta_data, 
                   pkt_type_t type, 
                   pkt_subtype_t subtype)
{
    
    
    if(type == PKT_DATA)
    {
        parse_data_pkt(pkt, meta_data);  
    }
    else if(type == PKT_MGMT)
    {
        if(subtype.mgmt_subtype == PKT_BEACON || subtype.mgmt_subtype == PKT_PROBE_RES)
        {
            parse_beacon_pkt(pkt, meta_data);
        }
    }
}

//*****************************************************************************
//  Public Functions
//*****************************************************************************

esp_err_t mac_logger_get_ap_list_len(uint8_t* n)
{
    if(_take_lock()) 
    { 
        *n = 0;
        return ESP_ERR_INVALID_STATE; 
    }

    *n = ap_list_len;
    _release_lock();
    return ESP_OK;
}

esp_err_t mac_logger_get_ap(uint8_t ap_index, ap_t* ap)
{
    if(_take_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(ap_index >= ap_list_len)
    {
        ESP_LOGE(TAG, "invalid AP index");
        _release_lock();
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(ap, ap_list + ap_index, sizeof(ap_t));
    
    _release_lock();
    return ESP_OK;
}

esp_err_t mac_logger_init(void)
{
    if(!one_time_init_done)
    {
        lock = xSemaphoreCreateBinary();
        assert(xSemaphoreGive(lock) == pdTRUE);
        one_time_init_done = 1;
        ESP_LOGI(TAG, "lock inited");
    }

    pkt_sniffer_filtered_src_t f = {0};
    f.cb = mac_logger_cb;
    pkt_subtype_t x;
    x.data_subtype = PKT_DATA_DATA;
    pkt_sniffer_add_type_subtype(&f, PKT_DATA, x);
    x.data_subtype = PKT_QOS_DATA;
    pkt_sniffer_add_type_subtype(&f, PKT_DATA, x);
    x.mgmt_subtype = PKT_BEACON;
    pkt_sniffer_add_type_subtype(&f, PKT_MGMT, x);
    x.mgmt_subtype = PKT_PROBE_RES;
    pkt_sniffer_add_type_subtype(&f, PKT_MGMT, x);
    
    esp_err_t e = pkt_sniffer_add_filter(&f);
    ESP_LOGI(TAG, "Type Mask = 0x%x   Data Mask = 0x%x   MGMT Mask = 0x%x\n", f.filter.type_bitmap, f.filter.data_subtype_bitmap, f.filter.mgmt_subtype_bitmap);
    return e;
}

esp_err_t mac_logger_clear(void)
{
    if(_take_lock()) {return ESP_ERR_INVALID_STATE; }

    clear_ap_list();

    ESP_LOGI(TAG, "Cleared Lists");
    _release_lock();
    return ESP_OK;
}