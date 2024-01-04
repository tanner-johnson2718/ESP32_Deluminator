#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_wifi.h"

static const char* TAG = "PKT SNIFFER";

static uint8_t num_filters = 0;
static uint8_t pkt_sniffer_running = 0;
pkt_sniffer_filtered_cb_t filtered_cbs[CONFIG_PKT_MAX_FILTERS];
static SemaphoreHandle_t lock;

//*****************************************************************************
// First line CB code and EAPOL Packet Parsing
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

static inline int16_t get_seq_num(uint8_t* pkt, uint16_t len)
{
    if(len <= SEQ_NUM_UB)
    {
        ESP_LOGE(TAG, "in get seq num, pakt too small");
        return -1;
    }

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

// Returns 0-5 if its a packet in an EAPOL seq, else 255
static inline void eapol_pkt_parse(uint8_t* p, uint16_t len)
{
    uint8_t num = 255;

    if(get_type(p) == 0 && get_subtype(p) == 0)
    {
        // assoc request
        num = 0;
    }

    if(get_type(p) == 0 && get_subtype(p) == 1)
    {
        // assoc response
        num = 1;
    }


    if((len> 0x22) && (p[0x20] == 0x88) && (p[0x21] == 0x8e))
    {
        // eapol
        int16_t s = get_seq_num(p, len);
        uint8_t to = get_to_ds(p);
        uint8_t from = get_from_ds(p);

        if     (s == 0 && to == 0 && from == 1) { num = 2; }
        else if(s == 0 && to == 1 && from == 0) { num = 3; }
        else if(s == 1 && to == 0 && from == 1) { num = 4; }
        else if(s == 1 && to == 1 && from == 0) { num = 5; }
        else
        {
            ESP_LOGE(TAG, "WAAAHHH -> seq=%d   to_ds=%d   from_ds=%d", s, to, from);
            return 255;
        }
    }

    return num;
}

static void pkt_sniffer_cb(void* buff, wifi_promiscuous_pkt_type_t type)
{
   
    wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*) buff;

    uint8_t* dst = p->payload + 4;
    uint8_t* src = p->payload + 10;
    uint8_t* ap =  p->payload + 16;

}

//*****************************************************************************
// API Functions
//*****************************************************************************

uint8_t pkt_sniffer_is_running(void)
{
    return pkt_sniffer_is_running;
}

esp_err_t add_filtered_cb(pkt_sniffer_filtered_cb_t* f)
{
    if(!xSemaphoreTake(lock, 0))
    {
        ESP_LOGE(TAG, "Timeout trying to add filter to cb list");
        return ESP_ERR_TIMEOUT;
    }

    if(num_filters == CONFIG_PKT_MAX_FILTERS)
    {
        ESP_LOGE(TAG, "Filtered CB list full");
        return ESP_ERR_NOMEM;
    }

    memcpy(&filtered_cbs[num_filters], f, sizeof(pkt_sniffer_filtered_cb_t));

    assert(xSemaphoreGive(lock) == pdTRUE);
}

esp_err_t pkt_sniffer_launch(uint8_t channel, wifi_promiscuous_filter_t type_filter)
{
    if(channel < 1 || channel > 11)
    {
        ESP_LOGE(TAG, "Tried to launch with invalid channel");
        return ESP_ERR_INVALID_ARG;
    }

    if(pkt_sniffer_is_running)
    {
        ESP_LOGE("Tried to launch but is already running");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t e;

    if( (e = esp_wifi_set_promiscuous(1)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set to promiscious mode");
        return e;
    }
    if( (e = esp_wifi_set_promiscuous_filter(&type_filter)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set to promiscious filter");
        return e;
    }
    if( (e = esp_wifi_set_promiscuous_rx_cb(&pkt_sniffer_cb)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register first line cb");
        return e;
    }
    if( (e = esp_wifi_set_channel(c, WIFI_SECOND_CHAN_NONE)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register first line cb");
        return e;
    }

    pkt_sniffer_running = 1;

    ESP_LOGI(TAG, "Launched with %d filters", num_filters);

}