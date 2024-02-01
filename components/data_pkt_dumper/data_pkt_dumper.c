#include "data_pkt_dumper.h"
#include "pkt_sniffer.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char* TAG = "Data Packet Dumper";

static void dumper(void* pkt, void* meta_data, pkt_type_t type, pkt_subtype_t subtype)
{
    dot11_header_t* pkt_header = (dot11_header_t*) pkt;
    wifi_pkt_rx_ctrl_t* rx_ctrl = (wifi_pkt_rx_ctrl_t*) meta_data;

    esp_log_write(ESP_LOG_INFO, "", "DS Status = 0x%x\n", pkt_header->ds_status);
    esp_log_write(ESP_LOG_INFO, "", "Prot Flag = 0x%x\n", pkt_header->protect);
    esp_log_write(ESP_LOG_INFO, "", "ADDR 1    = "MACSTR"\n", MAC2STR(pkt_header->addr1));
    esp_log_write(ESP_LOG_INFO, "", "ADDR 2    = "MACSTR"\n", MAC2STR(pkt_header->addr2));
    esp_log_write(ESP_LOG_INFO, "", "ADDR 3    = "MACSTR"\n", MAC2STR(pkt_header->addr3));
    esp_log_write(ESP_LOG_INFO, "", "Pkt Len   = %d\n\n", rx_ctrl->sig_len);
}


esp_err_t data_pkt_dumper_init(data_pkt_subtype_t s)
{
    pkt_sniffer_filtered_src_t f = {0};
    f.cb = dumper;
    pkt_subtype_t x;
    x.data_subtype = s;
    pkt_sniffer_add_type_subtype(&f, PKT_DATA, x);
    
    esp_err_t e = pkt_sniffer_add_filter(&f);
    ESP_LOGI(TAG, "Type Mask = %x   Data Mask = %x   MGMT Mask = %x\n", f.filter.type_bitmap, f.filter.data_subtype_bitmap, f.filter.mgmt_subtype_bitmap);
    return e;
}