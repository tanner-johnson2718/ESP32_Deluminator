#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "repl.h"
#include "wifi.h"
#include "user_interface.h"
#include "conf.h"

#define MAC_LEN 6
#define MAX_SSID_LEN 32
#define SEQ_NUM_LB        0x16
#define SEQ_NUM_LB_MASK   0xF0
#define SEQ_NUM_LB_RSHIFT 0x4
#define SEQ_NUM_UB        0x17
#define SEQ_NUM_UB_MASK   0xFF
#define SEQ_NUM_UB_LSHIFT 0x8
#define TO_DS_BYTE        0x1
#define TO_DS_MASK        0x1
#define FROM_DS_BYTE      0x1
#define FROM_DS_MASK      0x2

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
// to crack that AP. Moreover, we call clear active macs before and after we
// start a new scan which flushes the full eapol packets to flash mem.
#define EAPOL_MAX_PKT_LEN 256
#define EAPOL_NUM_PKTS    4
#define EAPOl_SEMA_TIMEOUT_MS 10
static uint8_t eapol_buffer[EAPOL_MAX_PKT_LEN * EAPOL_NUM_PKTS];
static uint16_t eapol_pkt_lens[EAPOL_NUM_PKTS];
static SemaphoreHandle_t eapol_lock;

// We maintain a single timer to be used as a "poll n dump" function ie ever
// TIMER_DELAY_MS milliseconds execute a function that usually polls and sends
// data to the serial console or the lcd
static esp_timer_handle_t gp_timer;
static esp_timer_create_args_t gp_timer_args = 
{
    .callback = NULL,
    .name = "Wifi GP Timer"
};
static uint8_t gp_timer_running = 0;

static void _init_wifi()
{
    esp_netif_t *sta_netif = NULL;
    esp_netif_t *ap_netif = NULL;

    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    if(CREATE_AP_IF)
    {
        ap_netif = esp_netif_create_default_wifi_ap();
        assert(ap_netif);
    }
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_netif_get_mac(sta_netif, mac));
    ESP_LOGI(TAG, "STA if created -> %02x:%02x:%02x:%02x:%02x:%02x", 
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    if(CREATE_AP_IF)
    {
        ESP_ERROR_CHECK(esp_netif_get_mac(ap_netif, mac));
        ESP_LOGI(TAG, "AP if created -> %02x:%02x:%02x:%02x:%02x:%02x", 
                    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    }
}

//*****************************************************************************
// General Purpose Timer
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

static void clear_active_mac_list(void)
{
    if(!xSemaphoreTake(active_mac_list_lock, 0))
    {
        ESP_LOGI(TAG, "clear mac list failed ..  busy");
        return;
    }

    active_mac_list_len = 0;
    active_mac_target_ap = -1;
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

static inline int16_t get_seq_num(uint8_t* pkt, uint16_t len)
{
    if(len <= SEQ_NUM_UB)
    {
        ESP_LOGE(TAG, "in get seq num, pakt too small");
        return -1;
    }

    int16_t lb = (((int16_t)pkt[SEQ_NUM_LB]) & SEQ_NUM_LB_MASK) >> SEQ_NUM_LB_RSHIFT;
    int16_t ub = (((int16_t)pkt[SEQ_NUM_UB]) & SEQ_NUM_UB_MASK) << SEQ_NUM_UB_LSHIFT; 

    return lb + ub;
}

static inline uint8_t get_to_ds(uint8_t* pkt)
{
    return !(!(pkt[TO_DS_BYTE] & TO_DS_MASK));
}

static inline uint8_t get_from_ds(uint8_t* pkt)
{
    return !(!(pkt[FROM_DS_BYTE] & FROM_DS_MASK));
}

// -1 if not eapol, else packet nume in handshake
static inline void eapol_pkt_parse(uint8_t* p, uint16_t len)
{

    if((len> 0x22) && (p[0x20] == 0x88) && (p[0x21] == 0x8e))
    {
        uint8_t num = 0;
        int16_t s = get_seq_num(p, len);
        uint8_t to = get_to_ds(p);
        uint8_t from = get_from_ds(p);

        if     (s == 0 && to == 0 && from == 1) { num = 0; }
        else if(s == 0 && to == 1 && from == 0) { num = 1; }
        else if(s == 1 && to == 0 && from == 1) { num = 2; }
        else if(s == 1 && to == 1 && from == 0) { num = 3; }
        else
        {
            ESP_LOGE(TAG, "WAAAHHH -> seq=%d   to_ds=%d   from_ds=%d", s, to, from);
            return;
        }

        ESP_LOGI(TAG, "%s -- Recovered WPA2 (%d/4)", ap_info[active_mac_target_ap].ssid, num+1);

        if(!xSemaphoreTake(eapol_lock, EAPOl_SEMA_TIMEOUT_MS / portTICK_PERIOD_MS))
        {
            ESP_LOGE(TAG, "BAD VERY BAD .. EAPOL lock time out on handshake pkt %d/4 ... dropping pkt", num+1);
            return;
        }

        if(len >= EAPOL_MAX_PKT_LEN)
        {
            ESP_LOGE(TAG, "ERROR EAPOL packet too long");
            assert(xSemaphoreGive(eapol_lock));
        }

        eapol_pkt_lens[num] = len;
        memcpy(eapol_buffer + EAPOL_MAX_PKT_LEN*num, p, len);

        assert(xSemaphoreGive(eapol_lock));

    }
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

static int do_repl_scan_ap(int argc, char** argv)
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

static int do_repl_scan_mac_start(int argc, char** argv)
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

static int do_repl_scan_mac_stop(int argc, char** argv)
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
    update_line(1);
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
        push_to_line_buffer(2, "");
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
        strncpy(line, (char*) ap_info[active_mac_target_ap].ssid, 19);
        push_to_line_buffer(0, line);
        snprintf(line, 19, "%02x:%02x:%02x:%02x:%02x:%02x", get_nth_mac(active_mac_target_ap)[0], get_nth_mac(active_mac_target_ap)[1], get_nth_mac(active_mac_target_ap)[2],get_nth_mac(active_mac_target_ap)[3],get_nth_mac(active_mac_target_ap)[4],get_nth_mac(active_mac_target_ap)[5]);
        push_to_line_buffer(1, line);
        snprintf(line, 19, "RSSI = %03d   ", get_nth_rssi(active_mac_target_ap));
        push_to_line_buffer(2, line);
        push_to_line_buffer(3, "");
        update_display();

        start_report_rssi_timer();

        return;
    }

    if(!report_num_macs_timer_running && !selecting_target_mac && report_rssi_timer_running)
    {
        // This cancels our rssi timer and should bring us back to report num
        // mac state. Kill the report rssi timer, reset screen and launch the
        // num macs timers

        stop_report_rssi_timer();

        lock_cursor();
        home_screen_pos();
        strncpy(line, (char*) ap_info[active_mac_target_ap].ssid, 19);
        push_to_line_buffer(0, line);
        snprintf(line, 19, "MACS Found = %03d", active_mac_list_len);
        push_to_line_buffer(1, line);
        push_to_line_buffer(2, "");
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
    _init_wifi();
    active_mac_list_lock = xSemaphoreCreateBinary();
    eapol_lock = xSemaphoreCreateBinary();

    assert(xSemaphoreGive(active_mac_list_lock) == pdTRUE);
    assert(xSemaphoreGive(eapol_lock) == pdTRUE);
}

void register_wifi(void)
{
    register_no_arg_cmd("scan_ap", "Scan for all Wifi APs", &do_repl_scan_ap);
    register_no_arg_cmd("scan_mac_start", "Start a scan of stations on an AP: sta_scan_start <ap_index from scan>", &do_repl_scan_mac_start);
    register_no_arg_cmd("scan_mac_stop", "Stop a scan of stations on an AP", &do_repl_scan_mac_stop);
}

void ui_add_wifi(void)
{
    add_ui_cmd("Scan AP", ui_scan_ap_ini, ui_scan_ap_cb, ui_scan_ap_fini);
    add_ui_cmd("Scan MAC", ui_scan_mac_ini, ui_scan_mac_cb, ui_scan_mac_fini);
}