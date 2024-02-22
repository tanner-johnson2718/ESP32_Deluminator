#include <stdio.h>
#include <string.h>

#include "data_pkt_dumper.h"
#include "pkt_sniffer.h"
#include "pcap.h"

#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"

static const char* TAG = "Data Packet Dumper";

#define LOG_DUMPER 0
#define DISK_DUMPER 1

static FILE* f = NULL;

static int write_safe(void* buff, int num, char* prompt)
{
    size_t num_written = fwrite(buff, 1 , num, f);
    if(num_written != num)
    {
        ESP_LOGE(TAG, "Failed to wrtie %s", prompt);
        fclose(f);
        return 1;
    }
    ESP_LOGI(TAG, "%d bytes written to disk\n", num_written);
    return 0;
}

static int write_pkt_safe(void* buff, int num, char* prompt)
{
    pcap_pkthdr_t pkt_hdr = {0};
    pkt_hdr.caplen = num;
    pkt_hdr.len = num;

    if(write_safe(&pkt_hdr, sizeof(pcap_pkthdr_t), prompt)) {return 1;}
    if(buff)
    {
        if(write_safe(buff, num, prompt)) {return 1;}
    }

    return 0;
}

static void dumper(void* pkt, void* meta_data, pkt_type_t type, pkt_subtype_t subtype)
{
    wifi_pkt_rx_ctrl_t* rx_ctrl = (wifi_pkt_rx_ctrl_t*) meta_data;

    #if LOG_DUMPER
        dot11_header_t* pkt_header = (dot11_header_t*) pkt;
        esp_log_write(ESP_LOG_INFO, "", "DS Status = 0x%x\n", pkt_header->ds_status);
        esp_log_write(ESP_LOG_INFO, "", "Prot Flag = 0x%x\n", pkt_header->protect);
        esp_log_write(ESP_LOG_INFO, "", "ADDR 1    = "MACSTR"\n", MAC2STR(pkt_header->addr1));
        esp_log_write(ESP_LOG_INFO, "", "ADDR 2    = "MACSTR"\n", MAC2STR(pkt_header->addr2));
        esp_log_write(ESP_LOG_INFO, "", "ADDR 3    = "MACSTR"\n", MAC2STR(pkt_header->addr3));
        esp_log_write(ESP_LOG_INFO, "", "Pkt Len   = %d\n\n", rx_ctrl->sig_len);
    #endif

    #if DISK_DUMPER
        if(write_pkt_safe(pkt, rx_ctrl->sig_len, "")){ return; }
    #endif
}


esp_err_t data_pkt_dumper_init(data_pkt_subtype_t s, char* file_name)
{
    char path[33];

    snprintf(path, 32, "/spiffs/%.23s", file_name);
    ESP_LOGI(TAG, "Opening %s .. ", path);
    f = fopen(path, "w");
    if(!f)
    {
        ESP_LOGE(TAG, "Error opening file");
        return ESP_ERR_INVALID_STATE;
    }

    pcap_file_header_t file_hdr = {0};
    file_hdr.magic = PCAP_MAGIC;
    file_hdr.linktype = DOT11_LINK_TYPE;
    file_hdr.snaplen = 0xffff;
    file_hdr.version_major = 2;
    file_hdr.version_minor = 4;
    if(write_safe(&file_hdr, sizeof(pcap_file_header_t), "pcap header"))
    {
        ESP_LOGE(TAG, "Failed to write pcap header");
        fclose(f);
        return ESP_ERR_INVALID_STATE;
    }

    pkt_sniffer_filtered_src_t filt = {0};
    filt.cb = dumper;
    pkt_subtype_t x;
    x.data_subtype = s;
    pkt_sniffer_add_type_subtype(&filt, PKT_DATA, x);
    
    esp_err_t e = pkt_sniffer_add_filter(&filt);
    ESP_LOGI(TAG, "Type Mask = 0x%x   Data Mask = 0x%x   MGMT Mask = 0x%x\n", filt.filter.type_bitmap, filt.filter.data_subtype_bitmap, filt.filter.mgmt_subtype_bitmap);
    return e;
}

esp_err_t data_pkt_dumper_fini(void)
{
    fclose(f);
    return ESP_OK;
}