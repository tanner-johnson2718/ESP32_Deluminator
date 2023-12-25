#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_console.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "repl.h"
#include "wifi.h"
#include "user_interface.h"

static const char* TAG = "WIFI";

// AP Poller Config
#define SCAN_CONF_SHOW_HIDDEN 1
#define SCAN_CONF_SCAN_TYPE WIFI_SCAN_TYPE_ACTIVE
#define SCAN_CONF_PASSIVE_SCAN_TIME 360
#define SCAN_CONF_ACTIVE_MAX 100
#define MAX_SSID_LEN 32
#define BSSID_LEN 6

static wifi_conf_t conf;
static wifi_scan_config_t scan_conf = {0};
static uint8_t scan_ssid[MAX_SSID_LEN + 1] = {0};
static uint8_t scan_bssid[BSSID_LEN] = {0};
static wifi_ap_record_t* ap_info;
static uint16_t ap_count = 0;
TaskHandle_t ap_poll_handle = 0;

static void _init_wifi()
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    scan_conf.show_hidden = SCAN_CONF_SHOW_HIDDEN;
    scan_conf.scan_type = SCAN_CONF_SCAN_TYPE;
    scan_conf.scan_time.passive = SCAN_CONF_PASSIVE_SCAN_TIME;
    scan_conf.scan_time.active.min = 0;
    scan_conf.scan_time.active.max = SCAN_CONF_ACTIVE_MAX;
    scan_conf.ssid = NULL;
    scan_conf.bssid = NULL;
    scan_conf.channel = 0;
    scan_conf.home_chan_dwell_time = 0;
}

static void update_ap_info()
{
    uint16_t max_ap_count = conf.scan_list_size;
    memset(ap_info, 0, sizeof(wifi_ap_record_t) * conf.scan_list_size);
    esp_wifi_scan_start(&scan_conf, true);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&max_ap_count, ap_info));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    
    if(ap_count >= conf.scan_list_size)
    {
        ap_count = conf.scan_list_size;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_stop());
}

static void set_scan_all()
{
    scan_conf.ssid = NULL;
    scan_conf.bssid = NULL;
    scan_conf.channel = 0;
}

static int32_t find_ap(char* ssid)
{
    int32_t i;
    for(i = 0; i < ap_count; ++i)
    {
        if(!strcmp(ssid, (char*)ap_info[i].ssid))
        {
            return i;
        }
    }

    return -1;
}

static int do_set_scan_target(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Usage: set_scan <all | ssid>");
        return 1;
    }

    wifi_mode_t mode;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&mode));

    set_scan_all();
    update_ap_info();

    if(strcmp(argv[1], "all") ==  0)
    {
        printf("Scan scope set to all\n");
        return 0;
    }

    int32_t i = find_ap(argv[1]);
    if(i < 0)
    {
        printf("Could not find ssid = %s, scan scope remains at all\n", argv[1]);
        return 1;
    }

    scan_conf.channel = ap_info[i].primary;
    strncpy((char*)scan_ssid,(char*) ap_info[i].ssid, MAX_SSID_LEN);
    memcpy(scan_bssid, ap_info[i].bssid, BSSID_LEN);

    scan_conf.ssid = scan_ssid;
    scan_conf.bssid = scan_bssid;

    printf("Scan scope set to %s\n", argv[1]);

    return 0;
}

static int do_dump_ap_info(int argc, char** argv)
{
    update_ap_info();

    int i;
    for(i = 0; i < ap_count; ++i)
    {
        printf("SSID=%-32s   BSSID=%02x:%02x:%02x:%02x:%02x:%02x   Channel=%02d   RSSI=%03d\n", ap_info[i].ssid, ap_info[i].bssid[0],ap_info[i].bssid[1],ap_info[i].bssid[2],ap_info[i].bssid[3],ap_info[i].bssid[4],ap_info[i].bssid[5], ap_info[i].primary, ap_info[i].rssi);
    }

    return 0;
}

static void ap_poll_task(void* arg)
{
    for(;;)
    {
        do_dump_ap_info(0, NULL);
        vTaskDelay(conf.ap_poll_delay_ms / portTICK_PERIOD_MS);
    }
}

static int do_scan_poll(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Usage: scan_poll <start | stop>\n");
        return 1;
    }

    int start = strcmp(argv[1], "start");
    int stop = strcmp(argv[1], "stop");

    if(start && stop)
    {
        printf("Usage: scan_poll <start | stop>\n");
        return 1;
    }

    if(!start)
    {
        if(ap_poll_handle != NULL)
        {
            printf("Scan poll already running\n");
            return 1;
        }

        xTaskCreate( ap_poll_task, "ap_poll_task", 2048, NULL, conf.ap_poll_prio, &ap_poll_handle );
        assert(ap_poll_handle != NULL);
        return 0;
    }

    if(!stop)
    {
        if(ap_poll_handle == NULL)
        {
            printf("Scan poll already stopped\n");
            return 1;
        }

        vTaskDelete( ap_poll_handle );
        ap_poll_handle = NULL;
        return 0;
    }

    return 0;
}

//*****************************************************************************
// UI SCAN CMD
//*****************************************************************************

void scan_on_press(uint8_t i)
{
    return;
}

void scan_ui_cmd(void)
{
    ESP_LOGI(TAG, "SCAN ALL UI CMD");
    update_ap_info();

    uint8_t i;
    uint8_t line_counter = 0;
    char line_buff[20] = {0};
    for(i = 0; i < ap_count; ++i)
    {
        strncpy(line_buff, (char*) ap_info[i].ssid, 19);
        push_to_line_buffer(line_counter, line_buff);
        line_counter++;
        ESP_LOGI(TAG, "%s", line_buff);

        sprintf(line_buff, "%02x:%02x:%02x:%02x:%02x:%02x", ap_info[i].bssid[0],ap_info[i].bssid[1],ap_info[i].bssid[2],ap_info[i].bssid[3],ap_info[i].bssid[4],ap_info[i].bssid[5]);
        push_to_line_buffer(line_counter, line_buff);
        line_counter++;
        ESP_LOGI(TAG, "%s", line_buff);

        sprintf(line_buff, "Channel=%02d", ap_info[i].primary);
        push_to_line_buffer(line_counter, line_buff);
        line_counter++;
        ESP_LOGI(TAG, "%s", line_buff);

        sprintf(line_buff, "RSSI=%02d", ap_info[i].rssi);
        push_to_line_buffer(line_counter, line_buff);
        line_counter++;
        ESP_LOGI(TAG, "%s", line_buff);

        line_buff[0] = (char) 0;
        push_to_line_buffer(line_counter, line_buff);
        line_counter++;
        ESP_LOGI(TAG, "%s", line_buff);
    }

}

//*****************************************************************************
// PUBLIC
//*****************************************************************************

void init_wifi(wifi_conf_t* _conf)
{
    memcpy(&conf, _conf, sizeof(wifi_conf_t));
    ap_info = malloc(_conf->scan_list_size*sizeof(wifi_ap_record_t));
    _init_wifi();
}

void register_wifi(void)
{
    register_no_arg_cmd("scan", "Scan for all Wifi APs", &do_dump_ap_info);
    register_no_arg_cmd("set_scan_target", "Set scope of scan: set_scan_target <all | ssid>", &do_set_scan_target);
    register_no_arg_cmd("scan_poll", "Poll ap scan: scan_poll <start | stop>", &do_scan_poll);
}

void ui_add_wifi(void)
{
    add_ui_cmd("scan all", scan_ui_cmd, scan_on_press);
}