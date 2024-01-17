#include <stdio.h>
#include <stdlib.h>
#include "string.h"

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "pkt_sniffer.h"

static const char* TAG = "PKT SNIFFER";

static uint8_t num_filters = 0;
static uint8_t inited = 0;
static uint8_t _pkt_sniffer_running = 0;
pkt_sniffer_filtered_src_t filtered_srcs[CONFIG_PKT_MAX_FILTERS];
static SemaphoreHandle_t lock;

//*****************************************************************************
// First line CB code
//*****************************************************************************

static inline uint8_t mac_is_eq(uint8_t* m1, uint8_t* m2)
{
    uint8_t i = 0;
    for(; i < 6; ++i)
    {
        if(m1[i]!=m2[i])
        {
            return 0;
        }
    }

    return 1;
}

static void pkt_sniffer_cb(void* buff, wifi_promiscuous_pkt_type_t type)
{
    pkt_sniffer_msg_t msg;
    wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*) buff;
    msg.p = p;
    msg.type = type;

    if(p->rx_ctrl.rx_state != 0)
    {
        // ESP_LOGI(TAG, "malformed packet");
        return;
    }

    uint8_t* dst = p->payload + 4;
    uint8_t* src = p->payload + 10;
    uint8_t* ap =  p->payload + 16;
 
    if(!xSemaphoreTake(lock, 0))
    {
        ESP_LOGE(TAG, "Timeout trying to parse packet");
        return;
    }

    uint8_t i;
    for(i = 0; i < num_filters; ++i)
    {
        if(filtered_srcs[i].ap_active &&  !mac_is_eq(ap, filtered_srcs[i].ap))
        {
            continue;
        }

        if(filtered_srcs[i].src_active &&  !mac_is_eq(src, filtered_srcs[i].src))
        {
            continue;
        }

        if(filtered_srcs[i].dst_active &&  !mac_is_eq(dst, filtered_srcs[i].dst))
        {
            continue;
        }

        if(!xQueueSend(filtered_srcs[i].q, (void*) &msg, 0))
        {
            printf("REPL MUX QUEUE FULL!!\n");
        }
    }

    assert(xSemaphoreGive(lock) == pdTRUE);
}

void _pkt_sniffer_init(void)
{
    lock = xSemaphoreCreateBinary();
    assert(xSemaphoreGive(lock) == pdTRUE);
    inited = 1;
}

//*****************************************************************************
// API Functions
//*****************************************************************************

uint8_t pkt_sniffer_is_running(void)
{
    return _pkt_sniffer_running;
}

esp_err_t pkt_sniffer_add_filter(pkt_sniffer_filtered_src_t* f)
{
    if(!inited)
    {
        _pkt_sniffer_init();
    }

    if(!xSemaphoreTake(lock, 0))
    {
        ESP_LOGE(TAG, "Timeout trying to add filter to list");
        return ESP_ERR_TIMEOUT;
    }

    if(num_filters == CONFIG_PKT_MAX_FILTERS)
    {
        ESP_LOGE(TAG, "Filtered CB list full");
        return ESP_ERR_NO_MEM;
    }

    memcpy(&filtered_srcs[num_filters], f, sizeof(pkt_sniffer_filtered_src_t));
    ++num_filters;
    assert(xSemaphoreGive(lock) == pdTRUE);

    ESP_LOGI(TAG, "Filtered CB added (%d/%d)", num_filters, CONFIG_PKT_MAX_FILTERS);

    return ESP_OK;
}

esp_err_t pkt_sniffer_clear_filter_list(void)
{
    if(!inited)
    {
        _pkt_sniffer_init();
    }

    if(!xSemaphoreTake(lock, 0))
    {
        ESP_LOGE(TAG, "Timeout trying to add filter to list");
        return ESP_ERR_TIMEOUT;
    }
    
    num_filters = 0;
    assert(xSemaphoreGive(lock) == pdTRUE);

    ESP_LOGI(TAG, "Filtered CB List Cleared");

    return ESP_OK;
}

esp_err_t pkt_sniffer_launch(uint8_t channel, wifi_promiscuous_filter_t type_filter)
{
    if(!inited)
    {
        _pkt_sniffer_init();
    }

    if(channel < 1 || channel > 11)
    {
        ESP_LOGE(TAG, "Tried to launch with invalid channel");
        return ESP_ERR_INVALID_ARG;
    }

    if(_pkt_sniffer_running)
    {
        ESP_LOGE(TAG, "Tried to launch but is already running");
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
    if( (e = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register first line cb");
        return e;
    }

    _pkt_sniffer_running = 1;

    ESP_LOGI(TAG, "Launched with %d/%d filters", num_filters, CONFIG_PKT_MAX_FILTERS);

    return ESP_OK;
}

esp_err_t pkt_sniffer_kill(void)
{
    if(!inited)
    {
        _pkt_sniffer_init();
    }

    if(!_pkt_sniffer_running)
    {
        ESP_LOGE(TAG, "Killed but not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Killed");
    _pkt_sniffer_running = 0;
    return esp_wifi_set_promiscuous(0);
}