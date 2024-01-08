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

//*****************************************************************************
// Lock Helpers
//*****************************************************************************

uint8_t _take_lock(void)
{
    if(!xSemaphoreTake(lock, CONFIG_EAPOL_LOGGER_WAIT_MS / portTICK_PERIOD_MS))
    {
        ESP_LOGE(TAG, "lock timeout");
        return 1;
    }

    return 0;
}

void _release_lock(void)
{
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

//*****************************************************************************
// Public API
//*****************************************************************************

void eapol_logger_cb(wifi_promiscuous_pkt_t* p, 
                     wifi_promiscuous_pkt_type_t type, 
                     WPA2_Handshake_Index_t eapol)
{

    uint16_t len = p->rx_ctrl.sig_len;
    uint8_t index = 0;
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

esp_err_t eapol_logger_init(void)
{
    pkt_sniffer_filtered_cb_t f = {0};
    f.cb = eapol_logger_cb;
    f.eapol_only = 1;

    lock = xSemaphoreCreateBinary();
    assert(xSemaphoreGive(lock) == pdTRUE);

    esp_err_t e = pkt_sniffer_add_filter(&f);
    ESP_LOGI(TAG, "inited");
    return e;
}

//*****************************************************************************
// REPL Test interface
//*****************************************************************************

int do_eapol_logger_init(int argc, char** argv)
{
    ESP_ERROR_CHECK(eapol_logger_init());
    return 0;
}