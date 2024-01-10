// High level system overview...

#include <stdio.h>
#include <dirent.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cmd_nvs.h"
#include "heap_memory_layout.h"

#include "user_interface.h"
#include "pkt_sniffer.h"
#include "tcp_file_server.h"
#include "mac_logger.h"
#include "eapol_logger.h"

static const char* TAG = "MAIN";

//*****************************************************************************
// The first main component of main is flash memory. To get an idea of how we
// utilize flash memory in this application below is the current flash layout
// although refer to paritions_example.csv for the most update reference:
//
// |--------------------------------------------------|
// |        Addr Range     |           Desc           |
// |-----------------------|--------------------------|
// | 0x00_0000 - 0x00_0FFF |      All `0xFF`s         | 
// | 0x00_1000 - 0x00_8FFF | Second Stage Boot loader | 
// | 0x00_9000 - 0x00_AFFF |      Partition Table     | 
// | 0x00_A000 - 0x00_AFFF |       Phy Init Data      |
// | 0x00_B000 - 0x01_FFFF |             NVS          |
// | 0x02_0000 - 0x11_FFFF |     Application image    |
// | 0x12_0000 -     <end> |      SPIFFS parition     |
// |--------------------------------------------------|
//
// The regions flash that our application interfaces with is NVS and SPIFFS. 
// NVS is rather simple and allows us store simple key pairs in the NVS 
// partition. The esp_wifi module requires it otherwise our code does not. The
// SPIFFS is a SPI Flash File system. Is has a flat dir structure and no dirs
// are allowed. Once inited, one can use the C standard Library functions to 
// create, write, and read files from the system. The main files we will store
// are:
//
//     * /spiffs/history.txt - REPL command line history
//     * /spiffs/event.txt   - Event Loop Debug info
//     * /spiffs/<ssid>.pkt  - Packet Dump of WPA2 handshakes
//
//*****************************************************************************

#define MOUNT_PATH CONFIG_SPIFFS_MOUNT_PATH
#define MAX_FILES CONFIG_SPIFFS_MAX_FILES
#define PROMPT_STR CONFIG_REPL_PROMPT_STR
#define MAX_CMD_LINE_LEN CONFIG_MAX_CMD_LINE_LEN 
#define HISTORY_PATH MOUNT_PATH "/history.txt"
#define MAX_HISTORY_LEN CONFIG_MAX_HISTORY_LEN

static void initialize_filesystem(void);
static void initialize_nvs(void);

//*****************************************************************************
// One of the main components of the ESP32 Deluminator is a thin wrapper for 
// the esp idf console library. This provides a REPL over the USB serial 
// console which cna be accesed by running idf.py -p /dev/ttyUSB0 monitor. 
// 
// We use the base API but do not use its arg parsing frame work and we impose
// arg parsing on the writer of the repl func. To register a REPL function use
// register_no_arg_cmd(...) on a function with the signature:
//                    int f(int argc, char** argv) 
// 
// Moreover this component gives us logging via ESP_LOGI/E/W/etc. over the
// usb serial console. menuconfig and the logging API give means to adjust the
// outpul level globaly and specific to a module.
//
// We mainly use this REPL interface as a means of exporting test drivers that
// can be driven over the serial console and dumps its output over the serial
// console
//*****************************************************************************
static int do_dump_event_log(int, char**);
static int do_cat(int, char**);
static int do_ls(int, char**);
static int do_df(int, char**);
static int do_part_table(int, char**);
static int do_dump_soc_regions(int, char**);
static int do_free(int, char**);
static int do_restart(int, char**);
static int do_tasks(int, char**);
static void register_no_arg_cmd(char* cmd_str, char* desc, void* func_ptr);


//*****************************************************************************
// Wifi ... todo doc
//*****************************************************************************
#define EXAMPLE_ESP_WIFI_SSID "Linksys-76fc"
#define EXAMPLE_ESP_WIFI_CHANNEL 1
#define EXAMPLE_ESP_WIFI_PASS "abcd1234"
#define EXAMPLE_MAX_STA_CONN 1

static void init_wifi(void);

//*****************************************************************************
// External LCD Apps. We gave each app its own C file in the main dir and just
// register them ui system here
//*****************************************************************************`
extern void lcd_fsexp_init(void);
extern void lcd_fsexp_cb(uint8_t index);
extern void lcd_fsexp_fini(void);
extern void lcd_signal_sniffer_init(void);
extern void lcd_signal_sniffer_cb(uint8_t index);
extern void lcd_signal_sniffer_fini(void);

void app_main(void)
{
    // Dont mix up this order ... it matters
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    initialize_nvs();
    initialize_filesystem();
    ui_init();
    init_wifi();

    // ESP IDF REPL Func Registration
    esp_console_register_help_command();
    register_nvs();   // comes from esp-idf/examples/console/advanced

    // Some misc system level repl functions defined below
    register_no_arg_cmd("part_table", "Print the partition table", &do_part_table);
    register_no_arg_cmd("ls", "List files on spiffs", &do_ls);
    register_no_arg_cmd("df", "Disk free on spiffs", &do_df);
    register_no_arg_cmd("cat", "cat contents of file", &do_cat);
    register_no_arg_cmd("dump_event_log", "Dump the event log to disk: dump_event_log <file>", &do_dump_event_log);
    register_no_arg_cmd("soc_regions", "Print Tracked RAM regions: soc_regions <all|free> <cond|ext>", &do_dump_soc_regions);
    register_no_arg_cmd("tasks", "Print List of Tasks", &do_tasks);
    register_no_arg_cmd("free", "Print Available Heap Mem", &do_free);
    register_no_arg_cmd("restart", "SW Restart", &do_restart);

    // UI test driver repl functions
    register_no_arg_cmd("rotL", "Simulate rotating rotary left", &do_rot_l);
    register_no_arg_cmd("rotR", "Simulate rotating rotary right", &do_rot_r);
    register_no_arg_cmd("press", "Simulate short press", &do_press);
    register_no_arg_cmd("pressss", "Simulate long press", &do_long_press);

    // Pkt Sniffer / Mac Logger test driver repl functions
    register_no_arg_cmd("pkt_sniffer_add_filter", "Add a filter to the pkt sniffer", &do_pkt_sniffer_add_filter);
    register_no_arg_cmd("pkt_sniffer_launch", "Launch pkt sniffer on all types", &do_pkt_sniffer_launch);
    register_no_arg_cmd("pkt_sniffer_kill", "Kill pkt sniffer", &do_pkt_sniffer_kill);
    register_no_arg_cmd("pkt_sniffer_clear", "Clear the list of filters", &do_pkt_sniffer_clear);
    register_no_arg_cmd("mac_logger_start_dump", "Start Mac dump", &do_mac_logger_start_dump);
    register_no_arg_cmd("mac_logger_stop_dump", "Stop Mac dump", &do_mac_logger_stop_dump);
    register_no_arg_cmd("mac_logger_init", "Register the Mac logger cb with pkt sniffer and init module", &do_mac_logger_init);
    register_no_arg_cmd("eapol_logger_init", "Register the eapol logger with the pkt sniffer", &do_eapol_logger_init);

    // TCP File Server test driver repl functions
    register_no_arg_cmd("tcp_file_server_launch", "Launch the TCP File server, mount path as arg", &do_tcp_file_server_launch);
    register_no_arg_cmd("tcp_file_server_kill", "Kill the TCP File server", &do_tcp_file_server_kill);

    // register our ui apps
    ESP_ERROR_CHECK(ui_add_cmd("FS EXP" , lcd_fsexp_init, lcd_fsexp_cb, lcd_fsexp_fini));
    ESP_ERROR_CHECK(ui_add_cmd("Singal Hound v6.9", lcd_signal_sniffer_init, lcd_signal_sniffer_cb, lcd_signal_sniffer_fini));

    // Start the REPL
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_repl_t *repl = NULL;

    repl_config.prompt = PROMPT_STR;
    repl_config.max_cmdline_length = MAX_CMD_LINE_LEN;
    repl_config.history_save_path = HISTORY_PATH;
    repl_config.max_history_len = MAX_HISTORY_LEN;

    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));

    ESP_LOGI(TAG, "REPL Starting. Saving history too %s", HISTORY_PATH);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

//*****************************************************************************
// Init NVS and SPIFFS
//*****************************************************************************

static void initialize_filesystem(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS -> %s", MOUNT_PATH);

    esp_vfs_spiffs_conf_t _conf = {
      .base_path = MOUNT_PATH,
      .partition_label = NULL,
      .max_files = MAX_FILES,
      .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&_conf);

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
    ret = esp_spiffs_info(_conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s). Formatting...", esp_err_to_name(ret));
        esp_spiffs_format(_conf.partition_label);
        return;
    } else {
        ESP_LOGI(TAG, "%s mounted on partition size: total: %d, used: %d",MOUNT_PATH, total, used);
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

//*****************************************************************************
// Init Wifi Module
//*****************************************************************************
void init_wifi(void)
{
        // ESP NET IF init
    esp_netif_t *sta_netif = NULL;
    esp_netif_t *ap_netif = NULL;
    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);
    assert(sta_netif);
    
    // Wifi early init config (RX/TX buffers etc)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Handle dem wifi events on the default event loop
    /*
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    */

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                    .required = false,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_netif_get_mac(sta_netif, mac));
    ESP_LOGI(TAG, "STA if created -> %02x:%02x:%02x:%02x:%02x:%02x", 
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    ESP_ERROR_CHECK(esp_netif_get_mac(ap_netif, mac));
    ESP_LOGI(TAG, "AP if created -> %02x:%02x:%02x:%02x:%02x:%02x", 
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    // active_mac_list_lock = xSemaphoreCreateBinary();
    // eapol_lock = xSemaphoreCreateBinary();

    // assert(xSemaphoreGive(active_mac_list_lock) == pdTRUE);
    // assert(xSemaphoreGive(eapol_lock) == pdTRUE);

    // ESP_ERROR_CHECK_WITHOUT_ABORT(ui_add_cmd("Scan AP", ui_scan_ap_ini, ui_scan_ap_cb, ui_scan_ap_fini));
    // ESP_ERROR_CHECK_WITHOUT_ABORT(ui_add_cmd("Deauth Attack", ui_scan_mac_ini, ui_scan_mac_cb, ui_scan_mac_fini));
}

//*****************************************************************************
// Misc REPL Command
//*****************************************************************************

static struct 
{
    struct arg_end *end;
} no_args;

static void register_no_arg_cmd(char* cmd_str, char* desc, void* func_ptr)
{
    no_args.end = arg_end(1);
    const esp_console_cmd_t cmd = {
        .command = cmd_str,
        .help = desc,
        .hint = NULL,
        .func = func_ptr,
        .argtable = &no_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));

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

static int do_dump_event_log(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Usage: dump_event_log <file>\n");
        return 1;
    }

    const char* path = argv[1];
    FILE* f = fopen(path, "w");

    ESP_ERROR_CHECK(esp_event_dump(f));

    fclose(f);
    return 0;
}

static int do_dump_soc_regions(int argc, char **argv)
{
    /*
    typedef struct {
        intptr_t start;  ///< Start address of the region
        size_t size;            ///< Size of the region in bytes
        size_t type;             ///< Type of the region (index into soc_memory_types array)
        intptr_t iram_address; ///< If non-zero, is equivalent address in IRAM
    } soc_memory_region_t;
    */

    if(argc != 3)
    {
        printf("Usage soc_regions <all | free> <ext | cond>\n");
        return 1;
    }

    size_t num_regions = soc_get_available_memory_region_max_count();
    soc_memory_region_t _regions[num_regions];
    const soc_memory_region_t* regions;
    int i;
    const soc_memory_region_t *b;
    const soc_memory_region_t *a;
    size_t size = 0;

    if(argv[1][0] == 'a')
    {
        num_regions = soc_memory_region_count;
        regions = soc_memory_regions;

    }
    else if(argv[1][0] == 'f')
    {
        num_regions = soc_get_available_memory_regions(_regions);
        regions = _regions;
    }
    else
    {
        printf("Usage soc_regions <all | free> <ext | cond>\n");
        return 1;
    }

    if(argv[2][0] == 'e')
    {
        
        for(i = 0; i < num_regions ; ++i)
        {
            b = &regions[i];   
            printf("Start = 0x%x   Size = 0x%x   Type = %-6s   IRAM Addr = 0x%x\n",
               b->start, b->size, soc_memory_types[b->type].name, b->iram_address);
        }
    }
    else if(argv[2][0] == 'c')
    {
        a = &regions[0];
        size = a->size;
        for(i = 1; i < num_regions ; ++i)
        {
            b = &regions[i];

            // Found D/IRAM type assume to hit discontigous
            if((b->type == 1) && (a->type != 1))
            {
                printf("Start = 0x%x   Size = 0x%x   Type = %-6s\n",
                       a->start, size, soc_memory_types[a->type].name);
                a = b;
                size = a->size;
                continue;
            }
            else if(a->type == 1)
            {
                printf("Start = 0x%x   Size = 0x%x   Type = %-6s   IRAM Addr = 0x%x\n",
                        a->start, a->size, soc_memory_types[a->type].name, a->iram_address);
                a = b;
                size = a->size;
                continue;
            }

            // Found contig region
            if((a->start + size == b->start) && (a->type == b->type))
            {
                size += b->size;
                continue;
            }

            // Found Dis cont, print and reset a
            else
            {
                printf("Start = 0x%x   Size = 0x%x   Type = %-6s\n",
               a->start, size, soc_memory_types[a->type].name);
               a=b;
               size = a->size;
               continue;
            }
        }

        printf("Start = 0x%x   Size = 0x%x   Type = %-6s\n",
                     a->start, size, soc_memory_types[a->type].name);
    }
    else
    {
        printf("Usage soc_regions <all | free> <ext | cond>\n");
        return 1;
    }

    return 0;
}

static int do_tasks(int argc, char **argv)
{
    const size_t bytes_per_task = 40; /* see vTaskList description */
    char *task_list_buffer = malloc(uxTaskGetNumberOfTasks() * bytes_per_task);
    if (task_list_buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate buffer for vTaskList output");
        return 1;
    }
    fputs("Task Name\tStatus\tPrio\tHWM\tTask#", stdout);
#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
    fputs("\tAffinity", stdout);
#endif
    fputs("\n", stdout);
    vTaskList(task_list_buffer);
    fputs(task_list_buffer, stdout);
    free(task_list_buffer);
    return 0;
}

static int do_free(int argc, char **argv)
{
    printf("%"PRIu32"\n", esp_get_free_heap_size());
    return 0;
}

static int do_restart(int argc, char **argv)
{
    ESP_LOGI(TAG, "Restarting");
    esp_restart();
}