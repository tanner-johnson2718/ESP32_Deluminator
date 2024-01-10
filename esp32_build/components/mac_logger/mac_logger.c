#include <string.h>
#include <stdio.h>

#include "mac_logger.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"

static SemaphoreHandle_t lock;
static sta_t sta_list[CONFIG_MAC_LOGGER_MAX_STAS];
static int16_t sta_list_len = 0;
static ap_t ap_list[CONFIG_MAC_LOGGER_MAX_APS];
static int8_t ap_list_len = 0;
static uint8_t one_time_init_done = 0;

static const char* TAG = "MAC LOGGER";

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
// Private STA List Accessor Functions
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

static inline uint8_t* get_nth_mac(int16_t n) { return (uint8_t*) (sta_list + n); }
static inline int8_t get_nth_rssi(int16_t n){ return sta_list[n].rssi; }

static inline void set_nth_mac(int16_t n, uint8_t* m1)
{

    uint8_t i;
    for(i = 0; i < MAC_LEN; ++i)
    {  
        get_nth_mac(n)[i] = m1[i];
    }

}

static inline void set_nth_rssi(int16_t n, int8_t rssi)
{
    sta_list[n].rssi = rssi;
}

static inline void set_nth_ap_index(int16_t n, int8_t _ap_index)
{
    sta_list[n].ap_list_index = _ap_index;
}

static inline void insert(uint8_t* m1, int8_t rssi)
{
    if(sta_list_len == CONFIG_MAC_LOGGER_MAX_STAS)
    {
        ESP_LOGE(TAG, "STA list full");
        return;
    }

    if(_take_lock()) { return; }

    int16_t i = 0;
    for(; i < sta_list_len; ++i)
    {
        if(mac_is_eq(m1, get_nth_mac(i)))
        {
            set_nth_rssi(i, rssi);
            _release_lock();
            return;
        }
    }

    ++sta_list_len;
    set_nth_mac(sta_list_len-1, m1);    
    set_nth_rssi(sta_list_len-1, rssi);
    set_nth_ap_index(sta_list_len-1, -1);
    _release_lock();
}

//*****************************************************************************
// Private AP list funcs
//*****************************************************************************

static inline uint8_t get_type(uint8_t* pkt)
{
    return (pkt[0] & 0x0c) >> 2;
}

static inline uint8_t get_subtype(uint8_t* pkt)
{
    return (pkt[0] >> 4) & 0x0F;
}

// call this if and only if the packet is a beacon or probe response
static inline void insert_ap(wifi_promiscuous_pkt_t* p)
{
    uint8_t* pkt = p->payload;

    if(ap_list_len == CONFIG_MAC_LOGGER_MAX_APS)
    {
        ESP_LOGE(TAG, "AP list full");
        return;
    }

    if(_take_lock()){return;}

    // search the sta list as it should be in there since we inserted the src
    // address of all in coming frames.
    int16_t i;
    for(i = 0; i < sta_list_len; ++i)
    {
        if(mac_is_eq(pkt+10, get_nth_mac(i)))
        {
            break;
        }
    }

    if(i == sta_list_len)
    {
        ESP_LOGE(TAG, "inserting AP for non existant station");
        _release_lock();
        return;
    }

    if(sta_list[i].ap_list_index != -1)
    {
        _release_lock();
        return;
    }

    if(pkt[0x24] == 0 && pkt[0x25] > 0)
    {
        // found the ssid
        ap_t* ap = ap_list+ ap_list_len;
        
        memcpy(ap->ssid, pkt+ 0x26, pkt[0x25]);
        ap->ssid[pkt[0x25]] = 0;
        ap->channel = p->rx_ctrl.channel;

        ap->sta_list_index = i;
        set_nth_ap_index(i, ap_list_len);

        ++ap_list_len;
    }

    _release_lock();
}

//*****************************************************************************
//  Public Functions
//*****************************************************************************

esp_err_t mac_logger_get_sta_list_len(int16_t* n)
{
    if(_take_lock()) 
    { 
        *n = 0;
        return ESP_ERR_INVALID_STATE; 
    }

    *n = sta_list_len;
    _release_lock();
    return ESP_OK;
}

esp_err_t mac_logger_get_ap_list_len(int8_t* n)
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

esp_err_t mac_logger_get_sta(int16_t sta_list_index, sta_t* sta)
{
    if(_take_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(sta_list_index >= sta_list_len || sta_list_index < 0)
    {
        ESP_LOGE(TAG, "invalid STA index");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(sta, sta_list + sta_list_index, sizeof(sta_t));
    _release_lock();
    return ESP_OK;
}

esp_err_t mac_logger_get_ap(int8_t ap_list_index, sta_t* sta, ap_t* ap)
{
    if(_take_lock())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(ap_list_index >= ap_list_len || ap_list_index < 0)
    {
        ESP_LOGE(TAG, "invalid AP index");
        return ESP_ERR_INVALID_ARG;
    }

    int16_t sta_list_index = ap_list[ap_list_index].sta_list_index;
    if(sta_list_index >= sta_list_len || sta_list_index < 0)
    {
        ESP_LOGE(TAG, "Invalid STA index pulled from AP %d", ap_list_index);
        _release_lock();
        return ESP_ERR_NOT_FOUND;
    }


    memcpy(sta, sta_list + sta_list_index, sizeof(sta_t));
    memcpy(ap, ap_list + ap_list_index, sizeof(ap_t));
    _release_lock();
    return ESP_OK;
}

void mac_logger_cb(wifi_promiscuous_pkt_t* p, 
                   wifi_promiscuous_pkt_type_t type)
{
    if(!(type == WIFI_PKT_DATA || type == WIFI_PKT_MGMT))
    {
        return;
    }

    uint8_t* src = p->payload + 10;

    insert(src, p->rx_ctrl.rssi);

    if(type == WIFI_PKT_MGMT && ((get_subtype(p->payload) == 8) || get_subtype(p->payload) == 5))
    {
        insert_ap(p);
    }

}

esp_err_t mac_logger_init(void)
{
    pkt_sniffer_filtered_cb_t f = {0};
    f.cb = mac_logger_cb;

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

esp_err_t mac_logger_clear(void)
{
    if(_take_lock()) {return ESP_ERR_INVALID_STATE; }

    sta_list_len = 0;
    ap_list_len = 0;

    ESP_LOGI(TAG, "Cleared Lists");
    _release_lock();
    return ESP_OK;
}

//*****************************************************************************
// REPL Test Driver Functions
//*****************************************************************************

void dump(void* args)
{
    int16_t n, i;
    int8_t n_ap;
    ap_t ap;
    sta_t sta;    
    ESP_ERROR_CHECK(mac_logger_get_sta_list_len(&n));
    
    printf("STA LIST: \n");
    for(i = 0; i < n; ++i)
    {
        ESP_ERROR_CHECK(mac_logger_get_sta(i, &sta));
        printf("%02d) "MACSTR"   rssi=%d   ap_index=%d\n",i, MAC2STR(sta.mac), sta.rssi, sta.ap_list_index);
    }
    printf("%d stas\n\n", n);

    ESP_ERROR_CHECK(mac_logger_get_ap_list_len(&n_ap));
    printf("AP LIST: \n");
    for(i = 0; i < n_ap; ++i)
    {
        ESP_ERROR_CHECK(mac_logger_get_ap(i, &sta, &ap));
        printf("%02d) %-20s   channel=%d   sta_index=%d\n", i, ap.ssid, ap.channel, ap.sta_list_index);
    }
    printf("%d aps\n\n", n_ap);
}

static esp_timer_handle_t dump_timer;
static esp_timer_create_args_t dump_timer_args = 
{
    .callback = dump,
    .name = "Wifi GP Timer"
};

int do_mac_logger_init(int argc, char** argv)
{
    mac_logger_init();
    return 0;
}

int do_mac_logger_start_dump(int argc, char** argv)
{
    ESP_ERROR_CHECK(esp_timer_create(&dump_timer_args, &dump_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(dump_timer, 3000000));
    return 0;
}

int do_mac_logger_stop_dump(int argc, char** argv)
{
    ESP_ERROR_CHECK(esp_timer_stop(dump_timer));
    ESP_ERROR_CHECK(esp_timer_delete(dump_timer));
    
    return 0;
}   

