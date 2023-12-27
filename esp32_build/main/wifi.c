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

// Maintain a list of APs in order of RSSI
static wifi_conf_t conf;
static wifi_scan_config_t scan_conf = {0};
static uint8_t scan_ssid[MAX_SSID_LEN + 1] = {0};
static uint8_t scan_bssid[BSSID_LEN] = {0};
static wifi_ap_record_t* ap_info;
static uint16_t ap_count = 0;

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

// Maintain an "active ap" mac list. That is we choose an AP to target and we
// log every STA MAC that is associated with that AP
#define MAC_LEN 6
static uint8_t* active_mac_list = NULL;
static uint8_t active_mac_list_len = 0;
static uint8_t target_ap = 0;
static uint8_t sta_scanner_running = 0;

static void _init_wifi()
{
    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    if(conf.create_ap_if)
    {
        ap_netif = esp_netif_create_default_wifi_ap();
        assert(ap_netif);
    }
    
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

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_netif_get_mac(sta_netif, mac));
    ESP_LOGI(TAG, "STA if created -> %02x:%02x:%02x:%02x:%02x:%02x", 
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    if(conf.create_ap_if)
    {
    ESP_ERROR_CHECK(esp_netif_get_mac(ap_netif, mac));
    ESP_LOGI(TAG, "AP if created -> %02x:%02x:%02x:%02x:%02x:%02x", 
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    }
}

//*****************************************************************************
// AP Scan Helper Funcs
//*****************************************************************************

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

static void _set_scan_target(int32_t i)
{
    scan_conf.channel = ap_info[i].primary;
    strncpy((char*)scan_ssid,(char*) ap_info[i].ssid, MAX_SSID_LEN);
    memcpy(scan_bssid, ap_info[i].bssid, BSSID_LEN);

    scan_conf.ssid = scan_ssid;
    scan_conf.bssid = scan_bssid;

    ESP_LOGI(TAG, "Scan scope set to %s\n", scan_conf.ssid);
}

//*****************************************************************************
// Station Scan helper funcs
//*****************************************************************************

static uint8_t mac_is_eq(uint8_t* m1, uint8_t* m2)
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

static uint8_t* get_nth_mac(uint8_t n)
{
    assert(n < active_mac_list_len);
    return active_mac_list + n*MAC_LEN;
}


static void insert_mac(uint8_t* m1)
{
    if(active_mac_list_len == conf.scan_list_size)
    {
        ESP_LOGE(TAG, "ACTIVE MAC LIST FULL");
        return;
    }

    uint8_t i = 0;
    for(; i < active_mac_list_len; ++i)
    {
        if(mac_is_eq(m1, get_nth_mac(i)))
        {
            return;
        }
    }

    for(i = 0; i < MAC_LEN; ++i)
    {  
        get_nth_mac(active_mac_list_len)[i] = m1[i];
    }

}

static void clear_active_mac_list()
{
    memset(active_mac_list, 0, conf.scan_list_size*MAC_LEN);
}

static void set_sta_scan_target(uint8_t ap_index)
{
    if(ap_index >= ap_count )
    {
        ESP_LOGE(TAG, "TRIED to set STA scan target to AP outside of range");
        return;
    }

    ESP_LOGI(TAG, "STA Scan target set to %s", ap_info[ap_index].ssid);
    target_ap = ap_index;

}

//*****************************************************************************
// UI SCAN CMD. Pretty straightforward lcd UI command. On init of this cmd we
// home the screen, clear it and put an indicator that we are scanning. And 
// lock the cursor cause we dont want anyone messing around. We then push the
// AP info to the buff, unlock cursor and return. Nothing to do for button cb
// or fini.
//*****************************************************************************

void lcd_dump_ap(uint8_t i, uint8_t* line_counter)
{
    char line_buff[20] = {0};

    strncpy(line_buff, (char*) ap_info[i].ssid, 19);
    push_to_line_buffer(*line_counter, line_buff);
    (*line_counter)++;
    ESP_LOGI(TAG, "%s", line_buff);

    snprintf(line_buff,19, "%02x:%02x:%02x:%02x:%02x:%02x", ap_info[i].bssid[0],ap_info[i].bssid[1],ap_info[i].bssid[2],ap_info[i].bssid[3],ap_info[i].bssid[4],ap_info[i].bssid[5]);
    push_to_line_buffer(*line_counter, line_buff);
    (*line_counter)++;
    ESP_LOGI(TAG, "%s", line_buff);

    snprintf(line_buff,19, "Channel=%02d", ap_info[i].primary);
    push_to_line_buffer(*line_counter, line_buff);
    (*line_counter)++;
    ESP_LOGI(TAG, "%s", line_buff);

    snprintf(line_buff,19, "RSSI=%03d", ap_info[i].rssi);
    push_to_line_buffer(*line_counter, line_buff);
    (*line_counter)++;
    ESP_LOGI(TAG, "%s", line_buff);

    line_buff[0] = ' ';
    line_buff[1] = (char) 0;
    push_to_line_buffer(*line_counter, line_buff);
    (*line_counter)++;
    ESP_LOGI(TAG, "%s", line_buff);
}

void scan_on_press(uint8_t i){}
void scan_ui_cmd_fini(void){}

void scan_ui_cmd(void)
{
    ESP_LOGI(TAG, "SCAN ALL UI CMD");

    home_screen_pos();
    push_to_line_buffer(0, "Scanning...");
    push_to_line_buffer(1, "");
    push_to_line_buffer(2, "");
    push_to_line_buffer(3, "");
    update_display();
    lock_cursor();
    
    set_scan_all();
    update_ap_info();

    uint8_t i;
    uint8_t line_counter = 0;
    for(i = 0; i < ap_count; ++i)
    {
        lcd_dump_ap(i, &line_counter);   // puts in line buffer
    }

    unlock_cursor();
    
    return;   // updates display upon return
}

//*****************************************************************************
// UI SCAN RSSI CMD
//
// INI - On init to the same thing as the base scan UI command but only list
//       the ssids.
//
// CB - When button pressed use the passed index to limit the scan scope to the
//      indicated ap. Home the screen and lock the cursor. We then give the 
//      semaphore and launch the polling task. If the task is already running
//      we indicate to it to kill itself and wait for the poll task to give
//      the semaphore back, verifying its destruction. Then do a scan as we did
//      on ini
//
// FINI - Simply make sure our poll task is dead
//*****************************************************************************

static uint8_t scan_rssi_running = 0;
static SemaphoreHandle_t xSemaphore = NULL;
static TaskHandle_t scan_rssi_task_handle = 0;

static void kill_scan_rssi_task(void)
{
    ESP_LOGI(TAG, "KILLING SCAN RSSI TASK");
    scan_rssi_running = 0;
    assert(xSemaphoreTake( xSemaphore, 2*conf.ap_poll_delay_ms / portTICK_PERIOD_MS ) == pdTRUE);
    ESP_LOGI(TAG, "KILLED SCAN RSSI TASK");
    
}

static void list_ssids_lcd(void)
{
    uint8_t i;
    char line_buff[20] = {0};
    for(i = 0; i < ap_count; ++i)
    {
        strncpy(line_buff, (char*) ap_info[i].ssid, 19);
        push_to_line_buffer(i, line_buff);
        ESP_LOGI(TAG, "%s", line_buff);
    }
}

static void scan_rssi_task()
{
    // grab
    assert(xSemaphoreTake( xSemaphore, conf.ap_poll_delay_ms / portTICK_PERIOD_MS ) == pdTRUE);

    char line_buff[20] = {0};
    for(;;)
    {
        update_ap_info();
        snprintf(line_buff,19, "RSSI=%03d", ap_info[0].rssi);
        push_to_line_buffer(3, line_buff);
        home_screen_pos();
        update_line(3);

        if(scan_rssi_running == 0) { break; }
        vTaskDelay(conf.ap_poll_delay_ms / portTICK_PERIOD_MS);
        if(scan_rssi_running == 0) { break; }

    }

    // release
    assert(xSemaphoreGive(xSemaphore) == pdTRUE);

    vTaskDelete(NULL);
}

static void scan_rssi_fini()
{
    if(scan_rssi_running)
    {
        kill_scan_rssi_task();
    }  
}

static void scan_rssi_on_press_cb(uint8_t index)
{

    if(!scan_rssi_running)
    {
        if(index >= ap_count)
        {
            ESP_LOGE(TAG, "scan_rssi_cb index out of range");
            return;
        }

        scan_rssi_running = 1;
        _set_scan_target(index);

        uint8_t line_counter = 0;
        lcd_dump_ap(index, &line_counter);
        home_screen_pos();
        lock_cursor();
        update_display();

        ESP_LOGI(TAG, "STARTING SCAN RSSI TASK");
        assert(xSemaphoreGive(xSemaphore) == pdTRUE);
        xTaskCreate( scan_rssi_task, "scan_rssi_task", 4096, NULL, conf.ap_poll_prio, &scan_rssi_task_handle );
        assert(scan_rssi_task_handle);
        
    }
    else
    {
        kill_scan_rssi_task();

        home_screen_pos();
        push_to_line_buffer(0, "Scanning ...");
        push_to_line_buffer(1, "");
        push_to_line_buffer(2, "");
        push_to_line_buffer(3, "");
        update_display();

        set_scan_all();
        update_ap_info();
        list_ssids_lcd();
        unlock_cursor();
    }

}

static void scan_rssi_ini()
{
    ESP_LOGI(TAG, "SCAN RSSI UI CMD");
    home_screen_pos();
    push_to_line_buffer(0, "Scanning ...");
    push_to_line_buffer(1, "");
    push_to_line_buffer(2, "");
    push_to_line_buffer(3, "");
    update_display();
    lock_cursor();

    set_scan_all();
    update_ap_info();
    list_ssids_lcd();
    unlock_cursor();
}

//*****************************************************************************
// REPL Scan Cmds
//*****************************************************************************

static TaskHandle_t ap_poll_handle = 0;

static int do_dump_ap_info(int argc, char** argv)
{
    update_ap_info();

    int i;
    for(i = 0; i < ap_count; ++i)
    {
        printf("%d - SSID=%-19s   BSSID=%02x:%02x:%02x:%02x:%02x:%02x   Channel=%02d   RSSI=%03d\n", i, ap_info[i].ssid, ap_info[i].bssid[0],ap_info[i].bssid[1],ap_info[i].bssid[2],ap_info[i].bssid[3],ap_info[i].bssid[4],ap_info[i].bssid[5], ap_info[i].primary, ap_info[i].rssi);
    }

    return 0;
}


static int do_set_scan_target(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Usage: set_scan <all | ssid>");
        return 1;
    }

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

    _set_scan_target(i);

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
// STA Scanner REPL CMD
//*****************************************************************************

static void sniffer(void* buff, wifi_promiscuous_pkt_type_t type)
{
    
}

static int do_sta_scan_start(int argc, char** argv)
{
    if(argc != 2)
    {
        ESP_LOGE(TAG, "Usage: sta_scan_start <ssid index>");
        return 1;
    }

    set_sta_scan_target((utin8_t) strtol(argv[1]));

    // Call a timer to push MAC list to serial

    // Set prom mode, set filter, reg cb
    esp_wifi_set_promiscuous(1);

    wifi_promiscuous_filter_t filt = 
    {
        .filter_mask=WIFI_PROMIS_FILTER_MASK_MGMT|WIFI_PROMIS_FILTER_MASK_DATA
    };

    esp_wifi_set_promiscuous_filter(&filt);

}

static int do_sta_scan_stop(int argc, char** argv)
{
    if(!sta_scanner_running)
    {
        ESP_LOGE(TAG, "STA Scanner not running");
        return 1;
    }

    // Kill timer

    // Reset Wifi driver to original state
    esp_wifi_set_promiscuous(0);
}

//*****************************************************************************
// PUBLIC
//*****************************************************************************

void init_wifi(wifi_conf_t* _conf)
{
    if(ap_info)
    {
        ESP_LOGE(TAG, "ERROR init wifi called twice");
        return;
    }

    memcpy(&conf, _conf, sizeof(wifi_conf_t));

    ap_info = malloc(_conf->scan_list_size*sizeof(wifi_ap_record_t));
    active_mac_list = malloc(MAC_LEN * _conf->scan_list_size);
    assert(ap_info);
    assert(active_mac_list);

    _init_wifi();
    xSemaphore = xSemaphoreCreateBinary();
    assert(xSemaphore);
}

void register_wifi(void)
{
    register_no_arg_cmd("scan", "Scan for all Wifi APs", &do_dump_ap_info);
    register_no_arg_cmd("set_scan_target", "Set scope of scan: set_scan_target <all | ssid>", &do_set_scan_target);
    register_no_arg_cmd("scan_poll", "Poll ap scan: scan_poll <start | stop>", &do_scan_poll);
}

void ui_add_wifi(void)
{
    add_ui_cmd("scan all", scan_ui_cmd, scan_on_press, scan_ui_cmd_fini);
    add_ui_cmd("scan rssi", scan_rssi_ini, scan_rssi_on_press_cb, scan_rssi_fini);
}