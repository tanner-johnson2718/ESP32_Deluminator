// Entry Point for the ESP32 Delum. Some Notes on design:
//
// * Should stick to an async / event oriented design
// * Should always use the default event loop semantics for async funcs
// * PINs, Max file size, etc should be preprocessor macros here
// * Every Module provides an init function that takes a module_conf_t type to pass defines
// * These conf structs get copied into the static global space of that module
// * Modules that wish to register repl commands export a register_module() func

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_console.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "heap_memory_layout.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "repl.h"
#include "user_interface.h"
#include "cmd_system.h"
#include "cmd_nvs.h"

static const char* TAG = "APP MAIN";

// REPL defines
#define PROMPT_STR "$~>"
#define MAX_CMD_LINE_LEN 80
#define HISTORY_PATH MOUNT_PATH "/history.txt"

// FS defines
#define MOUNT_PATH "/spiffs"
#define MAX_FILES 32

// User Interface defines
#define LCD_ADDR 0x27
#define SDA_PIN  19
#define SCL_PIN  18
#define LCD_COLS 20
#define LCD_ROWS 4
#define BUTTON_PIN 13

// AP Poller Config
#define DEFAULT_SCAN_LIST_SIZE 16
#define SCAN_CONF_SHOW_HIDDEN 1
#define SCAN_CONF_SCAN_TYPE WIFI_SCAN_TYPE_ACTIVE
#define SCAN_CONF_PASSIVE_SCAN_TIME 360
#define SCAN_CONF_ACTIVE_MAX 100
#define MAX_SSID_LEN 32
#define BSSID_LEN 6
#define AP_POLL_PRIO tskIDLE_PRIORITY
#define AP_POLL_DELAY_MS 500

static wifi_scan_config_t scan_conf = {0};
static uint8_t scan_ssid[MAX_SSID_LEN + 1] = {0};
static uint8_t scan_bssid[BSSID_LEN] = {0};
static wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE] = {0};
static uint16_t ap_count = 0;
TaskHandle_t ap_poll_handle = 0;

static void initialize_filesystem(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = MOUNT_PATH,
      .partition_label = NULL,
      .max_files = MAX_FILES,
      .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s). Formatting...", esp_err_to_name(ret));
        esp_spiffs_format(conf.partition_label);
        return;
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void init_wifi()
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

static int do_part_table(int argc, char** argv)
{
    esp_partition_iterator_t part_iter;
    const esp_partition_t* part;

    part_iter = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

    while(part_iter != NULL)
    {
        part = esp_partition_get(part_iter);

        printf("%-16s  0x%08lx  0x%08lx\n", part->label, part->address, part->size);

        part_iter = esp_partition_next(part_iter);
    }

    return 0;
}

static int do_ls(int argc, char** argv)
{ 
    
    DIR *d;
    struct dirent *dir;

    printf("%s\n", MOUNT_PATH);
    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            printf("   - %s\n", dir->d_name);
        }
        closedir(d);
    }
    return 0;
}

static int do_df(int argc, char **argv)
{
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    printf("Partition size: total: %d, used: %d\n", total, used);

    return 0;
}

static int do_cat(int argc, char **argv)
{
    if(argc != 2)
    {
        printf("Usage: cat <path>\n");
        return 1;
    }

    const char* path = argv[1];
    FILE* f = fopen(path, "r");
    char line[81];
    
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file");
        return 1;
    }
    
    while(fgets(line, 81, f))
    {
        printf("%s", line);
    }

    fclose(f);
    return 0;
}

static void update_ap_info()
{
    uint16_t max_ap_count = DEFAULT_SCAN_LIST_SIZE;
    memset(ap_info, 0, sizeof(ap_info));
    esp_wifi_scan_start(&scan_conf, true);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&max_ap_count, ap_info));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    assert(ap_count < DEFAULT_SCAN_LIST_SIZE);
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
        vTaskDelay(AP_POLL_DELAY_MS / portTICK_PERIOD_MS);
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

        xTaskCreate( ap_poll_task, "ap_poll_task", 2048, NULL, AP_POLL_PRIO, &ap_poll_handle );
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

void app_main(void)
{

    user_interface_conf_t ui_conf = {LCD_ADDR,
                                     SDA_PIN,
                                     SCL_PIN,
                                     LCD_ROWS,
                                     LCD_COLS,
                                     BUTTON_PIN
                                    };

    repl_conf_t repl_conf = {HISTORY_PATH, PROMPT_STR, MAX_CMD_LINE_LEN};

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    initialize_nvs();
    initialize_filesystem();
    init_repl(&repl_conf);
    init_user_interface(&ui_conf);
    init_wifi();

    /* Register commands */
    esp_console_register_help_command();
    register_system();
    register_nvs();
    register_user_interface();
    register_misc_cmds();
    register_no_arg_cmd("part_table", "Print the partition table", &do_part_table);
    register_no_arg_cmd("ls", "List files on spiffs", &do_ls);
    register_no_arg_cmd("df", "Disk free on spiffs", &do_df);
    register_no_arg_cmd("cat", "cat contents of file", &do_cat);
    register_no_arg_cmd("scan", "Scan for all Wifi APs", &do_dump_ap_info);
    register_no_arg_cmd("set_scan_target", "Set scope of scan: set_scan_target <all | ssid>", &do_set_scan_target);
    register_no_arg_cmd("scan_poll", "Poll ap scan: scan_poll <start | stop>", &do_scan_poll);

    start_repl();  // no return
}
