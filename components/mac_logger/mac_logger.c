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
}

//*****************************************************************************
// Packet Parsing Helpers
//*****************************************************************************

#define SEQ_NUM_LB        0x16
#define SEQ_NUM_LB_MASK   0xF0
#define SEQ_NUM_LB_RSHIFT 0x4
#define SEQ_NUM_UB        0x17
#define SEQ_NUM_UB_MASK   0xFF
#define SEQ_NUM_UB_LSHIFT 0x8
#define TO_DS_BYTE        0x1
#define TO_DS_MASK        0x1
#define FROM_DS_BYTE      0x1
#define FROM_DS_MASK      0x2

static inline uint8_t get_type(uint8_t* pkt)
{
    return (pkt[0] & 0x0c) >> 2;
}

static inline uint8_t get_subtype(uint8_t* pkt)
{
    return (pkt[0] >> 4) & 0x0F;
}

static inline int16_t get_seq_num(uint8_t* pkt)
{
    int16_t lb = (((int16_t)pkt[SEQ_NUM_LB]) & SEQ_NUM_LB_MASK) >> SEQ_NUM_LB_RSHIFT;
    int16_t ub = (((int16_t)pkt[SEQ_NUM_UB]) & SEQ_NUM_UB_MASK) << SEQ_NUM_UB_LSHIFT; 

    return lb + ub;
}

static inline uint8_t get_to_ds(uint8_t* pkt)
{
    return !(!(pkt[TO_DS_BYTE] & TO_DS_MASK));
}

static inline uint8_t get_from_ds(uint8_t* pkt)
{
    return !(!(pkt[FROM_DS_BYTE] & FROM_DS_MASK));
}

// assume type is known to be mgmt
static inline uint8_t is_beacon(uint8_t* p)
{
    return get_subtype(p) == 8;
}

// assume type is known to be mgmt
static inline uint8_t is_probe(uint8_t* p)
{
    return get_subtype(p) == 5;
}

// assume type is known to be mgmt
static inline uint8_t is_assoc_req(uint8_t* p)
{
    return get_subtype(p) == 0;
}

// assume type is known to be mgmt
static inline uint8_t is_assoc_res(uint8_t* p)
{
    return get_subtype(p) == 1;
}

// assume type is known to be data
static inline int8_t get_eapol_index(uint8_t* p, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    uint16_t len = rx_ctrl->sig_len;

    if((len> 0x22) && (p[0x20] == 0x88) && (p[0x21] == 0x8e))
    {
        // eapol
        int16_t s = get_seq_num(p);
        uint8_t to = get_to_ds(p);
        uint8_t from = get_from_ds(p);

        if     (s == 0 && to == 0 && from == 1) { return 0; }
        else if(s == 0 && to == 1 && from == 0) { return 1; }
        else if(s == 1 && to == 0 && from == 1) { return 2; }
        else if(s == 1 && to == 1 && from == 0) { return 3; }
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

//*****************************************************************************
// Parse Inbound Packets
//*****************************************************************************

void parse_beacon_pkt(uint8_t* p, wifi_pkt_rx_ctrl_t* rx_ctrl)
{
    if(rx_ctrl->sig_len < 0x26 + MAX_SSID_LEN)
    {
        return;
    }

    if(_take_lock()){return;}

    int8_t index;
    find_ap(p + 16, &index);

    if(index < 0)
    {   
        if(p[0x25] > 0)
        {
            create_ap((char*)(p + 0x26), p[0x25], p+16, rx_ctrl->channel, rx_ctrl->rssi);
        }    
    }
    else
    {
        update_ap_rssi(index, rx_ctrl->rssi);
    }

    _release_lock();
    return;
}

void parse_data_pkt(uint8_t* p, wifi_pkt_rx_ctrl_t* rx_ctrl)
{

    if(_take_lock()){ return; }

    int8_t ap_index;
    find_ap(p+16, &ap_index);
    
    if(ap_index < 0)
    {
        _release_lock();
        return;
    }

    int8_t sta_index;
    uint8_t* sta_mac;
    if(mac_is_eq(p+4, p+16)) { sta_mac = p + 10;}
    else                     { sta_mac = p + 04; }

    find_sta(sta_mac, ap_index, &sta_index);
    if(sta_index < 0) { create_sta(ap_index, sta_mac, rx_ctrl->rssi );       }
    else {              update_sta_rssi(ap_index, sta_index, rx_ctrl->rssi); }

    _release_lock();
    return;
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

void mac_logger_cb(void* pkt, 
                   void* meta_data, 
                   pkt_type_t type, 
                   pkt_subtype_t subtype)
{
    if(type == PKT_DATA)
    {
        int8_t eapol_index = get_eapol_index(pkt, meta_data);
        if(eapol_index != -1)
        {
            parse_eapol_pkt(eapol_index + 2, pkt, meta_data);
        }
        else
        {
            parse_data_pkt(pkt, meta_data);   
        }
    }
    else if(type == PKT_MGMT)
    {
        if(is_beacon(pkt) || is_probe(pkt))
        {
            parse_beacon_pkt(pkt, meta_data);
        }
        else if(is_assoc_req(pkt))
        {
            parse_eapol_pkt(0, pkt, meta_data);
        }
        else if(is_assoc_res(pkt))
        {
            parse_eapol_pkt(1, pkt, meta_data);
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

esp_err_t mac_logger_get_ap(uint8_t ap_index, ap_summary_t* ap)
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

    memcpy(ap->ssid, ap_list[ap_index].ssid, MAX_SSID_LEN);
    memcpy(ap->bssid, ap_list[ap_index].bssid, MAC_LEN);
    ap->channel = ap_list[ap_index].channel;
    ap->rssi = ap_list[ap_index].rssi;
    ap->eapol_written_out = ap_list[ap_index].eapol_written_out;
    ap->num_assoc_stas = ap_list[ap_index].num_assoc_stas;
    
    _release_lock();
    return ESP_OK;
}

esp_err_t mac_logger_get_sta(uint8_t ap_index, uint8_t sta_index, sta_t* sta)
{
    if(_take_lock()) { return ESP_ERR_INVALID_STATE; }


    if(ap_index >= ap_list_len)
    {
        ESP_LOGE(TAG, "invalid AP index");
        _release_lock();
        return ESP_ERR_INVALID_ARG;
    }

    if(sta_index >= ap_list[ap_index].num_assoc_stas)
    {
        ESP_LOGE(TAG, "invalid STA index");
        _release_lock();
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(sta, &(ap_list[ap_index].stas[sta_index]), sizeof(sta_t));

    _release_lock();
    return ESP_OK;
}

esp_err_t mac_logger_launch(void)
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
    x.data_subtype = PKT_DATA_ANY;
    pkt_sniffer_add_type_subtype(&f, PKT_DATA, x);
    x.mgmt_subtype = PKT_ASSOC_REQ;
    pkt_sniffer_add_type_subtype(&f, PKT_MGMT, x);
    x.mgmt_subtype = PKT_ASSOC_RES;
    pkt_sniffer_add_type_subtype(&f, PKT_MGMT, x);
    x.mgmt_subtype = PKT_BEACON;
    pkt_sniffer_add_type_subtype(&f, PKT_MGMT, x);
    x.mgmt_subtype = PKT_PROBE_RES;
    pkt_sniffer_add_type_subtype(&f, PKT_MGMT, x);
    

    esp_err_t e = pkt_sniffer_add_filter(&f);
    // ESP_LOGI(TAG, "Type Mask = %x   Data Mask = %x   MGMT Mask = %x\n", f.filter.type_bitmap, f.filter.data_subtype_bitmap, f.filter.mgmt_subtype_bitmap);
    return e;
}