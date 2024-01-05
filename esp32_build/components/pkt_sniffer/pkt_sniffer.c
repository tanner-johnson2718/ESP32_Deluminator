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

static void pkt_sniffer_cb(void* buff, wifi_promiscuous_pkt_type_t type)
{
   
    wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*) buff;

    uint8_t* dst = p->payload + 4;
    uint8_t* src = p->payload + 10;
    uint8_t* ap =  p->payload + 16;
    WPA2_Handshake_Index_t eapol = eapol_pkt_parse(p->payload, p->rx_ctrl.sig_len);

    if(!xSemaphoreTake(lock, 0))
    {
        ESP_LOGE(TAG, "Timeout trying to parse packet");
        return;
    }

    uint8_t i;
    for(i = 0; i < num_filters; ++i)
    {
        if(eapol == HS_NONE && filtered_cbs[i].eapol_only)
        {
            continue;
        }

        if(filtered_cbs[i].ap_active &&  !mac_is_eq(ap, filtered_cbs[i].ap))
        {
            continue;
        }

        if(filtered_cbs[i].src_active &&  !mac_is_eq(src, filtered_cbs[i].src))
        {
            continue;
        }

        if(filtered_cbs[i].dst_active &&  !mac_is_eq(dst, filtered_cbs[i].dst))
        {
            continue;
        }

        filtered_cbs[i].cb(p, type, eapol);
    }

    assert(xSemaphoreGive(lock) == pdTRUE);
}    uint8_t ret = 0;

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

esp_err_t pkt_sniffer_add_filter(pkt_sniffer_filtered_cb_t* f)
{
    if(!inited)
    {
        _pkt_sniffer_init();
    }

    if(!xSemaphoreTake(lock, 0))
    {
        ESP_LOGE(TAG, "Timeout trying to add filter to cb list");
        return ESP_ERR_TIMEOUT;
    }

    if(num_filters == CONFIG_PKT_MAX_FILTERS)
    {
        ESP_LOGE(TAG, "Filtered CB list full");
        return ESP_ERR_NO_MEM;
    }

    memcpy(&filtered_cbs[num_filters], f, sizeof(pkt_sniffer_filtered_cb_t));
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
        ESP_LOGE(TAG, "Timeout trying to add filter to cb list");
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

//*****************************************************************************
// REPL Test Functions
//*****************************************************************************

static void _cb(wifi_promiscuous_pkt_t* p, 
                wifi_promiscuous_pkt_type_t type, 
                WPA2_Handshake_Index_t eapol)
{
    if(eapol != HS_NONE)
    {
        printf("EAPOL %d/5\n", (int) eapol);
        return;
    }

    printf("TYPE=");
    switch(type)
    {
        case WIFI_PKT_MGMT:
            printf("Man ");
            break;
        case WIFI_PKT_CTRL:
            printf("Ctl ");
            break;
        case WIFI_PKT_DATA:
            printf("Dat ");
            break;
        case WIFI_PKT_MISC:
            printf("Mis ");
            break;
        default:
            break;
    }

    printf("STYPE=%d ", get_subtype(p->payload));

    uint8_t* dst = p->payload + 4;
    uint8_t* src = p->payload + 10;
    uint8_t* ap =  p->payload + 16;

    printf("DST="MACSTR" SRC="MACSTR" AP="MACSTR"\n", MAC2STR(dst),
                                                      MAC2STR(src),
                                                      MAC2STR(ap));

}

// 0 sucesses parse, 1 on sscanf fail
static int sscanf_helper(uint8_t* mac, char* s)
{
    uint8_t ret = sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", 
                        &mac[0],
                        &mac[1],
                        &mac[2],
                        &mac[3],
                        &mac[4],
                        &mac[5]
    );


    if(ret != 6)
    {
        printf("Failed to parse AP MAC: %s\n", s);
        return 1;
    }

    return 0;
}

int do_pkt_sniffer_add_filter(int argc, char** argv)
{
    if(argc != 5)
    {
        printf("Usage: pkt_sniffer_add_filter <AP MAC or NULL> <DST MAC or NULL> <SRC MAC or NULL> <eapool or NULL>");
        return 1;
    }

    pkt_sniffer_filtered_cb_t filt_cb = {0};
    if(strcmp(argv[1], "NULL") != 0)
    {
        filt_cb.ap_active = 1;
        if(sscanf_helper(filt_cb.ap, argv[1])) {return 1;}
    }

    if(strcmp(argv[2], "NULL") != 0)
    {
        filt_cb.dst_active = 1;
        if(sscanf_helper(filt_cb.dst, argv[2])) {return 1;}
    }

    if(strcmp(argv[3], "NULL") != 0)
    {
        filt_cb.src_active = 1;
        if(sscanf_helper(filt_cb.src, argv[3])) {return 1;}
    }

    if(strcmp(argv[4], "NULL") != 0)
    {
        filt_cb.eapol_only = 1;
    }

    filt_cb.cb = _cb;
    ESP_ERROR_CHECK(pkt_sniffer_add_filter(&filt_cb));

    return 0;

}

int do_pkt_sniffer_launch(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Usage: pkt_sniffer_launch <channel>");
    }

    wifi_promiscuous_filter_t filt = 
    {
        .filter_mask=WIFI_PROMIS_FILTER_MASK_ALL
    };

    ESP_ERROR_CHECK(pkt_sniffer_launch((uint8_t) strtol(argv[1], NULL,10), filt));

    return 0;
}

int do_pkt_sniffer_kill(int argc, char** argv)
{
    ESP_ERROR_CHECK(pkt_sniffer_kill());
    return 0;
}

int do_pkt_sniffer_clear(int argc, char** argv)
{
    ESP_ERROR_CHECK(pkt_sniffer_clear_filter_list());
    return 0 ;
}