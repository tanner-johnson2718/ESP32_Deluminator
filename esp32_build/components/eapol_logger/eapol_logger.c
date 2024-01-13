#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "eapol_logger.h"
#include "esp_log.h"

#define EAPOL_MAX_PKT_LEN 256
#define EAPOL_NUM_PKTS    6
#define MAX_SSID_LEN 32
#define MOUNT_PATH "/spiffs"

static uint8_t eapol_buffer[EAPOL_MAX_PKT_LEN * EAPOL_NUM_PKTS];
static uint16_t eapol_pkt_lens[EAPOL_NUM_PKTS];
static uint8_t eapol_pkts_captured = 0;
static SemaphoreHandle_t lock;
static const char* TAG = "EAPOL LOGGER";
static uint8_t one_time_init_done = 0;

enum WPA2_Handshake_Index
{
    WPA2_HS_ASSOC_REQ,
    WPA2_HS_ASSOC_RES,
    EAPOL_HS_1,
    EAPOL_HS_2,
    EAPOL_HS_3,
    EAPOL_HS_4,
    HS_NONE             // Identifies the lack of relavanet WPA2 Hand shake pkt
} typedef WPA2_Handshake_Index_t;

//*****************************************************************************
// Lock Helpers
//*****************************************************************************

static uint8_t _take_lock(void)
{
    if(!one_time_init_done)
    {
        ESP_LOGE(TAG, "In take lock, not inited");
        return 1;
    }

    if(!xSemaphoreTake(lock, CONFIG_EAPOL_LOGGER_WAIT_MS / portTICK_PERIOD_MS))
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
        ESP_LOGE(TAG, "In release lock, not inited");
        return;
    }

    assert(xSemaphoreGive(lock) == pdTRUE);
}

//*****************************************************************************
// Private
//*****************************************************************************

static void parse_ssid(char* ssid, int len)
{
    // This points to the assoc req which should have the ssid as a tagged
    // param
    uint8_t* pkt = eapol_buffer; 
    if(pkt[0x1c] != 0)
    {
        ESP_LOGE(TAG, "Failed to parse SSID from Assoc Req.");
        return;
    }

    uint8_t in_pkt_len = pkt[0x1d];
    if(in_pkt_len >= len-1)
    {
        in_pkt_len = len-1;
    }

    memcpy(ssid, pkt + 0x1e, in_pkt_len);
    ssid[in_pkt_len] = 0;
}

// Call me with dat lock boi
static void eapol_dump_to_disk(void)
{
    char path[MAX_SSID_LEN+1];
    char ssid[21] = {0};

    parse_ssid(ssid, 21);

    snprintf(path, MAX_SSID_LEN+1, "%s/%.21s.pkt", MOUNT_PATH, ssid);
    
    ESP_LOGI(TAG, "Opening %s to writeout eapol pkts", path);
    FILE* f = fopen(path, "w");
    if(!f)
    {
        ESP_LOGE(TAG, "Failed to open %s - %s", path, strerror(errno));
        fclose(f);
        return;
    }

    size_t num_written = fwrite(eapol_pkt_lens, 1, 2*EAPOL_NUM_PKTS, f);
    if(num_written != 2 * EAPOL_NUM_PKTS)
    {
        ESP_LOGE(TAG, "Failed to write EAPOL Header (%d / %d)", num_written, 2*EAPOL_NUM_PKTS);
        fclose(f);
        return;
    }

    uint8_t i;
    for(i = 0; i < EAPOL_NUM_PKTS; ++i)
    {
        num_written = fwrite(eapol_buffer + i*EAPOL_MAX_PKT_LEN, 1, eapol_pkt_lens[i], f);
        if(num_written != eapol_pkt_lens[i])
        {
            ESP_LOGE(TAG, "Failed to write EAPOL %d Pkt (%d / %d)", i, num_written, eapol_pkt_lens[i]);
            fclose(f);
            return;
        }
    }
    
    fclose(f);
    ESP_LOGI(TAG, "Write out of EAPOL pkts successful!");

    eapol_pkt_lens[0] = 0;
    eapol_pkt_lens[1] = 0;
    eapol_pkt_lens[2] = 0;
    eapol_pkt_lens[3] = 0;
    eapol_pkt_lens[4] = 0;
    eapol_pkt_lens[5] = 0;
    eapol_pkts_captured = 0;
}

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

static inline int16_t get_seq_num(uint8_t* pkt, uint16_t len)
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

static inline uint8_t get_type(uint8_t* pkt)
{
    return (pkt[0] & 0x0c) >> 2;
}

static inline uint8_t get_subtype(uint8_t* pkt)
{
    return (pkt[0] >> 4) & 0x0F;
}

static inline WPA2_Handshake_Index_t eapol_pkt_parse(uint8_t* p, uint16_t len)
{

    if(get_type(p) == 0 && get_subtype(p) == 0)
    {
        // assoc request
        return WPA2_HS_ASSOC_REQ;
    }

    if(get_type(p) == 0 && get_subtype(p) == 1)
    {
        // assoc response
        return WPA2_HS_ASSOC_RES;
    }


    if((len> 0x22) && (p[0x20] == 0x88) && (p[0x21] == 0x8e))
    {
        // eapol
        int16_t s = get_seq_num(p, len);
        uint8_t to = get_to_ds(p);
        uint8_t from = get_from_ds(p);

        if     (s == 0 && to == 0 && from == 1) { return EAPOL_HS_1; }
        else if(s == 0 && to == 1 && from == 0) { return EAPOL_HS_2; }
        else if(s == 1 && to == 0 && from == 1) { return EAPOL_HS_3; }
        else if(s == 1 && to == 1 && from == 0) { return EAPOL_HS_4; }
    }

    return HS_NONE;
}

//*****************************************************************************
// Public API
//*****************************************************************************

void eapol_logger_cb(wifi_promiscuous_pkt_t* p, 
                     wifi_promiscuous_pkt_type_t type)
{

    uint16_t len = p->rx_ctrl.sig_len;
    uint8_t index = 0;
    
    if(!(type == WIFI_PKT_DATA || type == WIFI_PKT_MGMT))
    {
        return;
    }

    WPA2_Handshake_Index_t eapol = eapol_pkt_parse(p->payload, len);
    if(eapol == HS_NONE)
    {
        return;
    }

    if(len >= EAPOL_MAX_PKT_LEN)
    {
        ESP_LOGE(TAG, "Recved pkt with len greater than %d", EAPOL_MAX_PKT_LEN);
        return;
    }
    switch(eapol)
    {
        case WPA2_HS_ASSOC_REQ:
            index = 0;
            break;
        case WPA2_HS_ASSOC_RES:
            index = 1;
            break;
        case EAPOL_HS_1:
            index = 2;
            break;
        case EAPOL_HS_2:
            index = 3;
            break;
        case EAPOL_HS_3:
            index = 4;
            break;
        case EAPOL_HS_4:
            index = 5;
            break;
        default:
            ESP_LOGE(TAG, "Recved non eapol packet, check filter");
            return;
    }

    if(_take_lock()) { return; }

    if(eapol_pkt_lens[index] != 0 || eapol_pkts_captured == EAPOL_NUM_PKTS)
    {
        ESP_LOGE(TAG, "Possibly recved duplicate eapol pkt or multiple handshakes at once");
        _release_lock();
        return;
    }

    memcpy(eapol_buffer + (index*EAPOL_MAX_PKT_LEN), p->payload, len);
    eapol_pkt_lens[index] = len;
    ESP_LOGI(TAG, "Eapol Captured (%d/6)", index);
        
    eapol_pkts_captured++;

    if(eapol_pkts_captured == EAPOL_NUM_PKTS)
    {
        eapol_dump_to_disk();
    }

    _release_lock();
}

esp_err_t eapol_logger_init(uint8_t* ap_mac)
{
    pkt_sniffer_filtered_cb_t f = {0};
    f.cb = eapol_logger_cb;

    if(ap_mac != NULL)
    {
        f.ap_active = 1;
        memcpy(f.ap, ap_mac, 6);
    }

    if(!one_time_init_done)
    {
        lock = xSemaphoreCreateBinary();
        assert(xSemaphoreGive(lock) == pdTRUE);
        one_time_init_done = 1;
        ESP_LOGI(TAG, "lock inited");
    }
    
    esp_err_t e = pkt_sniffer_add_filter(&f);
    ESP_LOGI(TAG, "filter added");
    return e;
}