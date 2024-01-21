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

int filter_match(uint8_t i, mgmt_header_t* hdr)
{
    uint8_t mask = (filtered_srcs[i].filter.type_bitmap);
    if(!(((uint8_t)1 << (uint8_t) hdr->type) & mask))
    {
        return 0;
    }

    uint16_t mask16;
    if(hdr->type == PKT_MGMT)
    {
        mask16 = filtered_srcs[i].filter.mgmt_subtype_bitmap;
    }
    else if(hdr->type == PKT_DATA)
    {
        mask16 = filtered_srcs[i].filter.mgmt_subtype_bitmap;
    }
    else
    {
        return 0;
    }

    if(!(( (uint16_t) 1 << (uint8_t) hdr->sub_type)  & mask16 ))
    {
        return 0;
    }

    return 1;
}

//*****************************************************************************
// First line CB code
//*****************************************************************************

static void pkt_sniffer_cb(void* buff, wifi_promiscuous_pkt_type_t type)
{
    wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*) buff;

    if(p->rx_ctrl.rx_state != 0){ return; }

    if(!xSemaphoreTake(lock, 0))
    {
        ESP_LOGE(TAG, "Timeout trying to parse packet");
        return;
    }

    mgmt_header_t* hdr = (mgmt_header_t*) buff;

    uint8_t i;
    for(i = 0; i < num_filters; ++i)
    {
        if(filter_match(i, hdr))
        {
            pkt_subtype_t x;
            x.mgmt_subtype = hdr->sub_type;
            filtered_srcs[i].cb((void*)p->payload,
                                (void*) &(p->rx_ctrl), 
                                (pkt_type_t) hdr->type, 
                                x);            
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

esp_err_t pkt_sniffer_add_type_subtype(pkt_sniffer_filtered_src_t* f, 
                                       pkt_type_t type, 
                                       pkt_subtype_t subtype)
{
    if((uint8_t) type > 16 || (uint8_t) subtype.mgmt_subtype > 16)
    {
        ESP_LOGE(TAG, "In pkt_sniffer_add_type_subtype type or sub type invalid");
        return ESP_ERR_INVALID_ARG;
    }

    
    if(type == PKT_MGMT || type == PKT_ANY)
    {
        if(subtype.mgmt_subtype == PKT_MGMT_ANY)
        {
            f->filter.mgmt_subtype_bitmap = 0xffff;
        }
        else
        {
            f->filter.mgmt_subtype_bitmap |= (1 << (uint8_t) subtype.mgmt_subtype);
        }   
    }
    else if(type == PKT_DATA || type == PKT_ANY)
    {
        if(subtype.data_subtype == PKT_DATA_ANY)
        {
            f->filter.data_subtype_bitmap = 0xffff;
        }
        else
        {
            f->filter.data_subtype_bitmap |= (1 << (uint8_t) subtype.data_subtype);
        }   
    }
    else
    {
        ESP_LOGE(TAG, "Invalid type passed to pkt_sniffer_add_type_subtype");
        return ESP_OK;
    }

    if(type == PKT_ANY)
    {
        f->filter.type_bitmap = 0xff;
    }
    else
    {
        f->filter.type_bitmap |= (1 << (uint8_t) type);
    }

    return ESP_OK;
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

esp_err_t pkt_sniffer_launch(uint8_t channel)
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
    wifi_promiscuous_filter_t filt = 
    {
        .filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA
    };

    if( (e = esp_wifi_set_promiscuous(1)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set to promiscious mode");
        return e;
    }
    if( (e = esp_wifi_set_promiscuous_filter(&filt)) != ESP_OK)
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