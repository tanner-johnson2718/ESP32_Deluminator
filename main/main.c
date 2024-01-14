// This is the starting point for the esp32 deluminator. The main module here
// inits all the important subsystems. These are)
//
//    * Flash memory w/ spiffs file system to store files
//    * Flash memory w/ NVS storage for the wifi system
//    * REPL serial interface for driving system
//    * Wifi in AP/STA mode so as to have an access point and station
//
// Next main registers all of our "services" or components to act on system
// events. These are the repl commands that allow one to drive the system via 
// the serial command line or over the supplied wifi AP. The system is composed 
// of the following high level modules:
//
//    * PKT Sniffer - Adds a layer extra filtering on top of the existing 
//                    promiscious wifi mode. Allows us to multiplex packets
//                    and have serveral services processing pkts they are
//                    intersted in.
//    * MAC Logger - Sits on top of PKT Sniffer and logs all STAs, APs, and
//                   their association.
//    * EAPOL Logger - Listens for WPA2 handshakes and dumps them to disk
//    * WSL Bypasser - Got from homie online (see component for details). 
//                     Allows us to send deauth pkts posing as a differnt AP
//    * TCP File Server - Serves up the WPA2 handshake packets stored in flash
//                        to requestors over the AP.
//    * REPL MUX - Provides multiplexing of logging and input to the repl.

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
#include "esp_mac.h"

#include "pkt_sniffer.h"
#include "tcp_file_server.h"
#include "mac_logger.h"
#include "eapol_logger.h"
#include "wsl_bypasser.h"
#include "repl_mux.h"

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
// One of the main components of the ESP32 Deluminator is a wrapper for 
// the esp idf console library. This provides a REPL over the USB serial 
// console which can be accesed by running idf.py -p /dev/ttyUSB0 monitor.
//
// We also added a REPL MUX layer on top of the esp logging and esp console
// modules. This allows us to 1) send logging data to multiple places and to
// interact with the repl over the created wifi access point.
//
// **NOTE**
//    - printf only sends traffic over UART
//    - ESP_LOGE, etc adds debug level, tag and time stamp and will send 
//      through the REPL MUX
//    - use esp_log_write for generic log messages that go through the mux
//      without adding all the extra stuff
// 
// We use the base API but do not use its arg parsing frame work and we impose
// arg parsing on the writer of the repl func. To register a REPL function use
// register_no_arg_cmd(...) on a function with the signature:
//                    int f(int argc, char** argv) 
// 
// The below functions interface with the API of our components and gives us
// a means of intercating with our system through the repl.
//*****************************************************************************
static int do_dump_event_log(int, char**);
static int do_cat(int, char**);
static int do_ls(int, char**);
static int do_df(int, char**);
static int do_rm(int argc, char** argv);

static int do_part_table(int, char**);
static int do_dump_soc_regions(int, char**);
static int do_free(int, char**);
static int do_restart(int, char**);
static int do_tasks(int, char**);
static int do_get_task(int argc, char** argv);

static int do_send_deauth(int, char**);
static int do_eapol_logger_init(int argc, char** argv);
static int do_mac_logger_init(int argc, char** argv);
static int do_mac_logger_dump(int argc, char** argv);
static int do_pkt_sniffer_add_filter(int argc, char** argv);
static int do_pkt_sniffer_launch(int argc, char** argv);
static int do_pkt_sniffer_kill(int argc, char** argv);
static int do_pkt_sniffer_clear(int argc, char** argv);

static void register_no_arg_cmd(char* cmd_str, char* desc, void* func_ptr);

//*****************************************************************************
// We configure the wifi such that it can both be a host and a client. Be sure
// that you do not run pkt sniffer with a client connected or it will fail.
//*****************************************************************************
#define EXAMPLE_ESP_WIFI_SSID "Linksys-76fc"
#define EXAMPLE_ESP_WIFI_CHANNEL 1
#define EXAMPLE_ESP_WIFI_PASS "abcd1234"
#define EXAMPLE_MAX_STA_CONN 1

static void init_wifi(void);

void app_main(void)
{
    // Dont mix up this order ... it matters
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    initialize_nvs();
    initialize_filesystem();
    init_wifi();
    ESP_ERROR_CHECK(repl_mux_init());

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
    register_no_arg_cmd("rm", "Delete all the files on the FS", &do_rm);
    register_no_arg_cmd("get_task", "Print name of current task", &do_get_task);

    // Pkt Sniffer / Mac Logger test driver repl functions
    register_no_arg_cmd("pkt_sniffer_add_filter", "Add a filter to the pkt sniffer", &do_pkt_sniffer_add_filter);
    register_no_arg_cmd("pkt_sniffer_launch", "Launch pkt sniffer on all types", &do_pkt_sniffer_launch);
    register_no_arg_cmd("pkt_sniffer_kill", "Kill pkt sniffer", &do_pkt_sniffer_kill);
    register_no_arg_cmd("pkt_sniffer_clear", "Clear the list of filters", &do_pkt_sniffer_clear);
    register_no_arg_cmd("mac_logger_dump", "dump mac data", &do_mac_logger_dump);
    register_no_arg_cmd("mac_logger_init", "Register the Mac logger cb with pkt sniffer and init module", &do_mac_logger_init);
    register_no_arg_cmd("eapol_logger_init", "Register the eapol logger with the pkt sniffer", &do_eapol_logger_init);
    register_no_arg_cmd("send_deauth", "send_deauth <ap_mac> <sta_mac>", &do_send_deauth);

    // TCP File Server test driver repl functions
    register_no_arg_cmd("tcp_file_server_launch", "Launch the TCP File server, mount path as arg", &do_tcp_file_server_launch);
    register_no_arg_cmd("tcp_file_server_kill", "Kill the TCP File server", &do_tcp_file_server_kill);

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
}

//*****************************************************************************
// REPL Command Helpers
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

//*****************************************************************************
// REPL Logger funcs
//*****************************************************************************

static int do_send_deauth(int argc, char** argv)
{
    if(argc != 3)
    {
        printf("Usage) send_deauth <ap_mac> <sta_mac>");
        return 1;
    }

    uint8_t ap_mac[6];
    uint8_t sta_mac[6];

    if(sscanf(argv[1], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", ap_mac, ap_mac+1, ap_mac+2, ap_mac+3, ap_mac+4, ap_mac+5) != 6)
    {
        printf("Error parsing ap_mac = %s\n", argv[1]);
        return 1;
    }

    if(sscanf(argv[2], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", sta_mac, sta_mac+1,sta_mac+2, sta_mac+3, sta_mac+4,sta_mac+5) != 6)
    {
        printf("Error parsing sta_mac = %s\n", argv[2]);
        return 1;
    }

    printf("Deauthing "MACSTR" from "MACSTR"\n", MAC2STR(sta_mac), MAC2STR(ap_mac));
    ESP_ERROR_CHECK(wsl_bypasser_send_deauth_frame_targted(ap_mac, sta_mac));

    return 0;
}

static void dump(void* args)
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
        printf("%02d) "MACSTR"   rssi=%d   ap_index=%d   assoc_index=%d\n",i, MAC2STR(sta.mac), sta.rssi, sta.ap_list_index, sta.ap_assoc_index);
    }
    printf("%d stas\n\n", n);

    ESP_ERROR_CHECK(mac_logger_get_ap_list_len(&n_ap));
    printf("AP LIST: \n");
    for(i = 0; i < n_ap; ++i)
    {
        ESP_ERROR_CHECK(mac_logger_get_ap(i, &sta, &ap));
        printf("%02d) %-20s   channel=%d   sta_index=%d   num_stas=%d\n", i, ap.ssid, ap.channel, ap.sta_list_index, ap.num_assoc_stas);
    }
    printf("%d aps\n\n", n_ap);
}

static int do_mac_logger_init(int argc, char** argv)
{
    mac_logger_init(NULL);
    return 0;
}

static int do_mac_logger_dump(int argc, char** argv)
{
    dump(NULL);
    return 0;
}

static int do_eapol_logger_init(int argc, char** argv)
{
    ESP_ERROR_CHECK(eapol_logger_init(NULL));
    return 0;
}

static inline uint8_t get_subtype(uint8_t* pkt)
{
    return (pkt[0] >> 4) & 0x0F;
}

static void _cb(wifi_promiscuous_pkt_t* p, 
                wifi_promiscuous_pkt_type_t type)
{

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

static int do_pkt_sniffer_add_filter(int argc, char** argv)
{
    if(argc != 4)
    {
        printf("Usage: pkt_sniffer_add_filter <AP MAC or NULL> <DST MAC or NULL> <SRC MAC or NULL>");
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

    filt_cb.cb = _cb;
    ESP_ERROR_CHECK(pkt_sniffer_add_filter(&filt_cb));

    return 0;

}

static int do_pkt_sniffer_launch(int argc, char** argv)
{
    if(argc != 2)
    {
        printf("Usage: pkt_sniffer_launch <channel>");
        return 1;
    }

    wifi_promiscuous_filter_t filt = 
    {
        .filter_mask=WIFI_PROMIS_FILTER_MASK_ALL
    };

    ESP_ERROR_CHECK(pkt_sniffer_launch((uint8_t) strtol(argv[1], NULL,10), filt));

    return 0;
}

static int do_pkt_sniffer_kill(int argc, char** argv)
{
    ESP_ERROR_CHECK(pkt_sniffer_kill());
    return 0;
}

static int do_pkt_sniffer_clear(int argc, char** argv)
{
    ESP_ERROR_CHECK(pkt_sniffer_clear_filter_list());
    return 0 ;
}

//*****************************************************************************
// FS repl funcs
//*****************************************************************************

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

static int do_rm(int argc, char** argv)
{
    DIR *d;
    struct dirent *dir;
    char path[33];

    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            snprintf(path, 33, "%s/%.22s", MOUNT_PATH, dir->d_name);
            printf("Removing %s\n",path);
            remove(path);
        }
        closedir(d);
    }

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

//*****************************************************************************
// Random system repl func
//*****************************************************************************

static int do_get_task(int argc, char** argv)
{
    char* name = pcTaskGetName(xTaskGetCurrentTaskHandle());
    printf("Current Task = %s\n", name);
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