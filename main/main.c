// This is the starting point for the esp32 deluminator. The main module here
// inits all the important esp32 idf subsystems. These are)
//
//    * Flash memory w/ spiffs file system to store files
//    * Flash memory w/ NVS storage for the wifi system
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
//                   their association. Will listen for WPA2 handshakes and
//                   dump them to disk
//    * WSL Bypasser - Got from homie online (see component for details). 
//                     Allows us to send deauth pkts posing as a differnt AP
//    * TCP File Server - Serves up the WPA2 handshake packets stored in flash
//                        to requestors over the AP.
//    * REPL MUX - Provides multiplexing of logging and input to the repl. Also
//                 provides command table (i.e. our own version of esp console)

#include <stdio.h>
#include <dirent.h>
#include <string.h>

#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "heap_memory_layout.h"
#include "esp_mac.h"
#include "esp_wifi.h"

#include "pkt_sniffer.h"
#include "tcp_file_server.h"
#include "mac_logger.h"
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
// create, write, and read files from the system. 
//
//*****************************************************************************

#define MOUNT_PATH CONFIG_SPIFFS_MOUNT_PATH
#define MAX_FILES CONFIG_SPIFFS_MAX_FILES

static void initialize_filesystem(void);
static void initialize_nvs(void);

//*****************************************************************************
// One of the main components of the ESP32 Deluminator is our own repl system
// that provides a means for registering commands and calling those commands
// from either a tcp socket or via UART.
// 
// The below functions interface with the API of our components and gives us
// a means of intercating with our system through the repl.
//*****************************************************************************
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
static int do_dump_wifi_stats(int argc, char** argv);

static int do_send_deauth(int, char**);

static int do_mac_logger_init(int argc, char** argv);
static int do_mac_logger_dump(int argc, char** argv);
static int do_mac_logger_clear(int argc, char** argv);

static int do_pkt_sniffer_launch(int argc, char** argv);
static int do_pkt_sniffer_kill(int argc, char** argv);
static int do_pkt_sniffer_clear(int argc, char** argv);
static int do_pkt_sniffer_launch_delayed(int argc, char** argv);

static int do_tcp_file_server_kill(int argc, char** argv);
static int do_tcp_file_server_launch(int argc, char** argv);

//*****************************************************************************
// We configure the wifi such that it can both be a host and a client. Be sure
// that you do not run pkt sniffer with a client connected or it will fail. We
// implement the following scheme. When a client connects we kill the packet
// sniffer if running. In the future if there are other services that cant be 
// ran concurrently with a client than we will kill them as well.
//
// We provide a command called pkt_sniffer_delay_launch. It works the same as
// the packet sniffer launch function but will wait until the client STA
// disconnects from the AP. Only one client can conect to the AP at time thus
// this ensures the disconnect event leaves the AP with no STAs
//*****************************************************************************
#define EXAMPLE_ESP_WIFI_SSID "Linksys-76fc"
#define EXAMPLE_ESP_WIFI_CHANNEL 1
#define EXAMPLE_ESP_WIFI_PASS "abcd1234"
#define EXAMPLE_MAX_STA_CONN 1

static uint8_t delayed_launch = 0;  // contains channel when set

static void init_wifi(void);

void app_main(void)
{
    // Dont mix up this order ... it matters
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    initialize_nvs();
    initialize_filesystem();
    init_wifi();

    // Some misc system level repl functions defined below
    repl_mux_register("part_table", "Print the partition table", &do_part_table);
    repl_mux_register("ls", "List files on spiffs", &do_ls);
    repl_mux_register("df", "Disk free on spiffs", &do_df);
    repl_mux_register("cat", "cat contents of file", &do_cat);
    repl_mux_register("soc_regions", "Print Tracked RAM regions: soc_regions <all|free> <cond|ext>", &do_dump_soc_regions);
    repl_mux_register("tasks", "Print List of Tasks", &do_tasks);
    repl_mux_register("free", "Print Available Heap Mem", &do_free);
    repl_mux_register("restart", "SW Restart", &do_restart);
    repl_mux_register("rm", "Delete all the files on the FS", &do_rm);
    repl_mux_register("get_task", "Print name of current task", &do_get_task);
    repl_mux_register("dump_wifi_stats", "Dump Wifi Stats <module>", &do_dump_wifi_stats);

    // Pkt Sniffer / Mac Logger test driver repl functions
    repl_mux_register("pkt_sniffer_launch", "Launch pkt sniffer on all types", &do_pkt_sniffer_launch);
    repl_mux_register("pkt_sniffer_kill", "Kill pkt sniffer", &do_pkt_sniffer_kill);
    repl_mux_register("pkt_sniffer_clear", "Clear the list of filters", &do_pkt_sniffer_clear);
    repl_mux_register("pkt_sniffer_launch_delayed", "Launch the pkt sniffer once client sta disconnected. Only run from TCP repl", &do_pkt_sniffer_launch_delayed);
    repl_mux_register("mac_logger_dump", "dump mac data", &do_mac_logger_dump);
    repl_mux_register("mac_logger_init", "Register the Mac logger cb with pkt sniffer and init the component", &do_mac_logger_init);
    repl_mux_register("mac_logger_clear", "Clear the AP and STA list of the mac logger", &do_mac_logger_clear);
    repl_mux_register("send_deauth", "send_deauth <ap_mac> <sta_mac>", &do_send_deauth);

    // TCP File Server test driver repl functions
    repl_mux_register("tcp_file_server_launch", "Launch the TCP File server, mount path as arg", &do_tcp_file_server_launch);
    repl_mux_register("tcp_file_server_kill", "Kill the TCP File server", &do_tcp_file_server_kill);

    ESP_ERROR_CHECK(repl_mux_init());
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

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) 
    {
        // wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;

        if(pkt_sniffer_is_running())
        {
            ESP_ERROR_CHECK_WITHOUT_ABORT(pkt_sniffer_kill());
        }
  
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) 
    {
        if(delayed_launch)
        {
            ESP_ERROR_CHECK_WITHOUT_ABORT(pkt_sniffer_launch(delayed_launch));
            delayed_launch = 0;
        }
    }
}

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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    

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
// REPL Logger funcs
//*****************************************************************************

static int do_send_deauth(int argc, char** argv)
{
    if(argc != 3)
    {
        esp_log_write(ESP_LOG_INFO, "","Usage) send_deauth <ap_mac> <sta_mac>");
        return 1;
    }

    uint8_t ap_mac[6];
    uint8_t sta_mac[6];

    if(sscanf(argv[1], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", ap_mac, ap_mac+1, ap_mac+2, ap_mac+3, ap_mac+4, ap_mac+5) != 6)
    {
        esp_log_write(ESP_LOG_INFO, "","Error parsing ap_mac = %s\n", argv[1]);
        return 1;
    }

    if(sscanf(argv[2], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", sta_mac, sta_mac+1,sta_mac+2, sta_mac+3, sta_mac+4,sta_mac+5) != 6)
    {
        esp_log_write(ESP_LOG_INFO, "","Error parsing sta_mac = %s\n", argv[2]);
        return 1;
    }

    esp_log_write(ESP_LOG_INFO, "","Deauthing "MACSTR" from "MACSTR"\n", MAC2STR(sta_mac), MAC2STR(ap_mac));
    ESP_ERROR_CHECK_WITHOUT_ABORT(wsl_bypasser_send_deauth_frame_targted(ap_mac, sta_mac));

    return 0;
}

static int do_mac_logger_init(int argc, char** argv)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_init());
    return 0;
}

static int do_mac_logger_dump(int argc, char** argv)
{
    uint8_t i, j, n;
    ap_summary_t ap;
    sta_t sta;

    ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_ap_list_len(&n));
    for(i = 0; i < n; ++i)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_ap(i, &ap));
        esp_log_write(ESP_LOG_INFO,"", "%-20s\n", ap.ssid);
        esp_log_write(ESP_LOG_INFO,"", MACSTR"\n", MAC2STR(ap.bssid));
        esp_log_write(ESP_LOG_INFO,"", "Channel = %d\n", ap.channel);
        esp_log_write(ESP_LOG_INFO,"", "RSSI = %d\n", ap.rssi);
        esp_log_write(ESP_LOG_INFO,"", "EAPOL Written to disk = %d\n", ap.eapol_written_out);
        esp_log_write(ESP_LOG_INFO,"", "Num Stas = %d\n", ap.num_assoc_stas);

        for(j = 0; j < ap.num_assoc_stas; ++j)
        {
            ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_sta(i,j,&sta));
            esp_log_write(ESP_LOG_INFO, "", "   "MACSTR" %d\n", MAC2STR(sta.mac), sta.rssi);
        }

        esp_log_write(ESP_LOG_INFO, "", "\n");
    } 
    
    
    return 0;
}

static int do_mac_logger_clear(int argc, char** argv)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_clear());
    return 0;
}

static int do_pkt_sniffer_launch(int argc, char** argv)
{
    if(argc != 2)
    {
        esp_log_write(ESP_LOG_INFO, "","Usage: pkt_sniffer_launch <channel>");
        return 1;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(pkt_sniffer_launch((uint8_t) strtol(argv[1], NULL,10)));

    return 0;
}

static int do_pkt_sniffer_launch_delayed(int argc, char** argv)
{
    if(argc != 2)
    {
        esp_log_write(ESP_LOG_INFO, "","Usage: pkt_sniffer_launch_delayed <channel>");
        return 1;
    }

    delayed_launch = ((uint8_t) strtol(argv[1], NULL,10));

    return 0;

}

static int do_pkt_sniffer_kill(int argc, char** argv)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(pkt_sniffer_kill());
    return 0;
}

static int do_pkt_sniffer_clear(int argc, char** argv)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(pkt_sniffer_clear_filter_list());
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

        esp_log_write(ESP_LOG_INFO, "","%-16s  0x%08lx  0x%08lx\n", part->label, part->address, part->size);

        part_iter = esp_partition_next(part_iter);
    }

    return 0;
}

static int do_ls(int argc, char** argv)
{ 
    
    DIR *d;
    struct dirent *dir;

    esp_log_write(ESP_LOG_INFO, "","%s\n", MOUNT_PATH);
    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            esp_log_write(ESP_LOG_INFO, "","   - %s\n", dir->d_name);
        }
        closedir(d);
    }
    return 0;
}

static int do_df(int argc, char **argv)
{
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    esp_log_write(ESP_LOG_INFO, "","Partition size: total: %d, used: %d\n", total, used);

    return 0;
}

static int do_cat(int argc, char **argv)
{
    if(argc != 2)
    {
        esp_log_write(ESP_LOG_INFO, "","Usage: cat <path>\n");
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
        esp_log_write(ESP_LOG_INFO, "","%s", line);
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
            esp_log_write(ESP_LOG_INFO, "","Removing %s\n",path);
            remove(path);
        }
        closedir(d);
    }

    return 0;
}

//*****************************************************************************
// Random system repl func
//*****************************************************************************

static int do_dump_wifi_stats(int argc, char** argv)
{
    if(argc != 2)
    {
        esp_log_write(ESP_LOG_INFO, "", "Usage: dump_wifi_stats <modules>");
        return 1;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_statis_dump((uint8_t) strtol(argv[1], NULL,10)));
    return 0;
}

static int do_get_task(int argc, char** argv)
{
    char* name = pcTaskGetName(xTaskGetCurrentTaskHandle());
    esp_log_write(ESP_LOG_INFO, "","Current Task = %s\n", name);
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
        esp_log_write(ESP_LOG_INFO, "","Usage soc_regions <all | free> <ext | cond>\n");
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
        esp_log_write(ESP_LOG_INFO, "","Usage soc_regions <all | free> <ext | cond>\n");
        return 1;
    }

    if(argv[2][0] == 'e')
    {
        
        for(i = 0; i < num_regions ; ++i)
        {
            b = &regions[i];   
            esp_log_write(ESP_LOG_INFO, "","Start = 0x%x   Size = 0x%x   Type = %-6s   IRAM Addr = 0x%x\n",
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
                esp_log_write(ESP_LOG_INFO, "","Start = 0x%x   Size = 0x%x   Type = %-6s\n",
                       a->start, size, soc_memory_types[a->type].name);
                a = b;
                size = a->size;
                continue;
            }
            else if(a->type == 1)
            {
                esp_log_write(ESP_LOG_INFO, "","Start = 0x%x   Size = 0x%x   Type = %-6s   IRAM Addr = 0x%x\n",
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
                esp_log_write(ESP_LOG_INFO, "","Start = 0x%x   Size = 0x%x   Type = %-6s\n",
               a->start, size, soc_memory_types[a->type].name);
               a=b;
               size = a->size;
               continue;
            }
        }

        esp_log_write(ESP_LOG_INFO, "","Start = 0x%x   Size = 0x%x   Type = %-6s\n",
                     a->start, size, soc_memory_types[a->type].name);
    }
    else
    {
        esp_log_write(ESP_LOG_INFO, "","Usage soc_regions <all | free> <ext | cond>\n");
        return 1;
    }

    return 0;
}

static int do_tasks(int argc, char **argv)
{
    const size_t bytes_per_task = 40; /* see vTaskList description */
    const size_t buff_size = uxTaskGetNumberOfTasks() * bytes_per_task;
    char *task_list_buffer = malloc(buff_size);
    if (task_list_buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate buffer for vTaskList output");
        return 1;
    }
    vTaskList(task_list_buffer);
    esp_log_write(ESP_LOG_INFO, "", "Task Name\tStatus\tPrio\tHWM\tTask#");
#ifdef CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
    esp_log_write(ESP_LOG_INFO, "","\tAffinity\n");
#else
    esp_log_write(ESP_LOG_INFO, "", "\n");
#endif

    size_t i;
    char line[bytes_per_task + 1];
    for(i = 0; i < buff_size; i=i+bytes_per_task)
    {
        memcpy(line,task_list_buffer + i, bytes_per_task );
        line[bytes_per_task] = 0;
        esp_log_write(ESP_LOG_INFO, "", line);
    }

    free(task_list_buffer);
    return 0;
}

static int do_free(int argc, char **argv)
{
    esp_log_write(ESP_LOG_INFO, "","%"PRIu32"\n", esp_get_free_heap_size());
    return 0;
}

static int do_restart(int argc, char **argv)
{
    ESP_LOGI(TAG, "Restarting");
    esp_restart();
}