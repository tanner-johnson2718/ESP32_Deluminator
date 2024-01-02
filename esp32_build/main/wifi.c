#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "repl.h"
#include "wifi.h"
#include "user_interface.h"
#include "conf.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

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

//*****************************************************************************
// We maintain an access point that we call the SoC AP to differentiate it
// from our ap list above that we get scanning. We only allow one station to
// connect at a time.
//
// SoC AP Funcs - The SoC AP we host has a handler pegged to the default event
// loop. When a station connects, this starts the state machine:
//
// |-------------|    |----------------------------|    |---------------------|
// | STA Connect |--->| Launch Client Handler Task |--->| Open Listening Port |
// |-------------|    |----------------------------|    |---------------------|
//                                                              |
//                                                              |
//                                                              |
//                                                              V
// |-----------------|    |----------------------|    |---------------------|
// | Fufil File Reqs |<---| Present Stored Files |<---|    Accept Conn      |
// |-----------------|    |----------------------|    |---------------------|
//          |                          ^                         ^ 
//          |                          |                         |
//          |---------------------------                         |
//          V                                                    |
// |--------------------------|                                  |
// | Handle Client Disconnect |----------------------------------- 
// |--------------------------|
//
//
// Failures) If anything happens in the first row i.e. we cant launch the conn 
//           handler or can't open the listening port, this indicates very bad 
//           system / config failures beyond our control. Assert it to be
//           successful and log n crash in failure.
//
//           Once the Listening port is opened, failures can be handled by the
//           catch all of "handle client disconnect" that frees up any client
//           resources and returns to the listening state.
//
//           A STA disconnect event will not interrupt the starting of the
//           client handler task or the starting of the listening port. Once
//           the listening port is open, a client disconnect will trigger a
//           graceful shutdown of the client handle thread and the listening
//           port.
//*****************************************************************************

//*****************************************************************************
// Client Data Structure Accessor Functions
//*****************************************************************************
enum client_state
{
    CLIENT_STA_NOT_CONN,
    CLIENT_STA_CONN,
    CLIENT_WAITING_ON_TCP_CONN,
    CLIENT_TCP_CONN
};

static const char* client_state_strs[] = 
{
    "Client Station Not Connected",
    "Client Station Connected to SoC AP",
    "Waiting for client to connect to TCP Server",
    "Client connected to TCP server"
};

struct client
{
    enum client_state state;
    uint8_t mac[MAC_LEN];
    uint8_t  id;
    TaskHandle_t handler_task;
    int socket;
    int listen_sock;
    char ip_addr[16];
} typedef client_t;

static client_t client = {0};

static enum client_state get_client_state(void)
{
    return client.state;
}

static void set_client_state(enum client_state s)
{
    ESP_LOGI(TAG, "%s", client_state_strs[(uint8_t) s]);
    client.state = s;
}

static uint8_t client_sta_is_conn(void)
{
    return ((uint8_t) get_client_state()) > 0;
}

static void set_client_soc_ap_id(uint8_t id)
{
    client.id = id;
}

static uint8_t get_client_soc_ap_id(void)
{
    return client.id;
}

static void set_client_mac(uint8_t* mac)
{
    memcpy(client.mac, mac, MAC_LEN);
}

static uint8_t* get_client_mac(void)
{
    return client.mac;
}

static void deauth_client(void)
{
    ESP_ERROR_CHECK(esp_wifi_deauth_sta(client.id));
    ESP_LOGI(TAG, "station "MACSTR" kicked, AID = %d", MAC2STR(get_client_mac()), get_client_soc_ap_id());  
}

//*****************************************************************************
// TCP Server Logic
//*****************************************************************************

// returns a 1 if the error caused should reset the tcp connection
static uint8_t handle_file_req(void)
{
    uint8_t rx_buffer[33];
    int len = recv(client.socket, rx_buffer, sizeof(rx_buffer) - 1, 0);
    
    if(len < 0)
    {
        ESP_LOGE(TAG, "In handle_file_req - error recv");
        return 1;
    }
    else if(len == 0)
    {
        ESP_LOGI(TAG, "In handle_file_req - session closed");
        return 1;
    }
    
    rx_buffer[len] = 0;
    if((char) rx_buffer[len-1] == '\n')
    {
        rx_buffer[len-1] = 0;
    }

    DIR *d;
    struct dirent *dir;

    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            if(strcmp(dir->d_name, (char*) rx_buffer) == 0)
            {
                break;
            }
        }

        closedir(d);
    }
    else
    {
        ESP_LOGE(TAG, "In present_files - failed to open %s", MOUNT_PATH);
        return 0;
    }

    if(dir == NULL)
    {
        ESP_LOGE(TAG, "In handle_file_req - requested non existent file %s", rx_buffer);
        return 0;
    }

    // Send file
    ESP_LOGI(TAG, "File %s requested", rx_buffer);
    uint8_t tx_buffer[256];
    char path[33];
    sprintf(path, "%s/%.22s", MOUNT_PATH, rx_buffer);
    FILE* f = fopen(path, "r");

    if(!f)
    {
        ESP_LOGE(TAG, "In handle_file_req - Failed to open %s", path);
        return 0;
    }

    size_t num_read = 0;
    int still_sending = 1;
    while(still_sending)
    {
        num_read = fread(tx_buffer, 1, 256, f);
        if(num_read < 256)
        {
            still_sending = 0;
        }

        ESP_LOGI(TAG, "Sending %d bytes ...", num_read);
        if( send(client.socket, tx_buffer, num_read, 0) < 1)
        {
            ESP_LOGE(TAG, "In handle_file_req - Failed to send file data");
            break;
        }
    }

    fclose(f);
    return 0;
}

// Returns a 1 if the error caused should reset the tcp connection
static uint8_t present_files(void)
{
    DIR *d;
    struct dirent *dir;

    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            uint8_t len = strlen(dir->d_name);
            int written =send(client.socket, dir->d_name, len, 0);
            if(written < 0)
            {
                ESP_LOGE(TAG, "In present Files - send error");
                return 1;
            }
            

            written = send(client.socket, "\n", 1, 0);
            if(written < 0)
            {
                ESP_LOGE(TAG, "In present Files - send error");
                return 1;
            }

            ESP_LOGI(TAG, "Successfully Presented file %s", dir->d_name);
        }
        closedir(d);
    }
    else
    {
        ESP_LOGE(TAG, "In present_files - failed to open %s", MOUNT_PATH);
        return 1;
    }

    return 0;
}

// Init the listening socket, bind it to our static IP and port
static void client_handler_task(void* args)
{

    client.listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(client.listen_sock < 0)
    {
        ESP_LOGE(TAG, "Failed to open listening socket");
        goto clean_up0;
    }

    int opt = 1;
    setsockopt(client.listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ESP_LOGI(TAG, "Listening Socket Created");

    struct sockaddr_storage dest_addr;
    struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ip4->sin_addr.s_addr = inet_addr(TCP_SERVER_IP);
    dest_addr_ip4->sin_family = AF_INET;
    dest_addr_ip4->sin_port = htons(TCP_SERVER_PORT);
    int err = bind(client.listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    err |= listen(client.listen_sock, 1);
    
    if(err)
    {
        ESP_LOGE(TAG, "Failed to bind listening socket");
        goto clean_up0;
    }

    ESP_LOGI(TAG, "Listening socket bound to %s:%d", TCP_SERVER_IP, TCP_SERVER_PORT);

    while(client.state != CLIENT_STA_NOT_CONN)
    {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);

        set_client_state(CLIENT_WAITING_ON_TCP_CONN);

        client.socket = accept(client.listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (client.socket < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: %s", strerror(errno));
            continue;
        }

        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, client.ip_addr, 16);
        set_client_state(CLIENT_TCP_CONN);
        ESP_LOGI(TAG, "Client Connected %s - Starting Session", client.ip_addr);

        while(client.state != CLIENT_STA_NOT_CONN)
        {
            if(present_files())
            {
                break;
            }

            if(handle_file_req())   // Generally block here
            {
                break;
            }
        }

        // Clean up sesion resources
        shutdown(client.socket, 0);
        close(client.socket);
    }

    // Clean up listening socket resources including this task. We force the
    // client to disconnect from the Soc AP here (if it that isnt what caused)
    // us to get here.
clean_up0:
    close(client.listen_sock);
    deauth_client();
    set_client_state(CLIENT_STA_NOT_CONN);
    ESP_LOGI(TAG, "Client Event Handler Task Exiting ...");
    vTaskDelete(NULL);
}

static void launch_client_handler_task(void)
{
    memset(&client.handler_task, 0, sizeof(TaskHandle_t));
    xTaskCreate(client_handler_task, "tcp_server", 4096, NULL, TCP_SERVER_PRIO, &client.handler_task);
    
    if(!client.handler_task)
    {
        ESP_LOGE(TAG,"Failed to start Client Handler Task");
        // set_client_state(CLIENT_STA_CONN);
    }

    ESP_LOGI(TAG, "Client Handler Task Launched");
}

//*****************************************************************************
// Wifi Event Loop Handler -> Handles action at the AP/STA level with regards
// to the client
//*****************************************************************************

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) 
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        
        // ESP_LOGI(TAG, "In wifi event handler - station "MACSTR" with id=%d connected to SoC AP", MAC2STR(get_client_mac()), get_client_soc_ap_id());

        set_client_mac(event->mac);
        set_client_soc_ap_id(event->aid);
        set_client_state(CLIENT_STA_CONN);
        launch_client_handler_task();
        
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) 
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "In wifi event handler - station "MACSTR" Diconnect Event, AID=%d", MAC2STR(event->mac), event->aid);

        // Just need to trigger the client handler task to clean up its shit
        set_client_state(CLIENT_STA_NOT_CONN );
        close(client.socket);
        close(client.listen_sock);

    }
    else
    {
        ESP_LOGI(TAG, "Unhandled WIFI event");
    }
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
        num_written = fwrite(eapol_buffer, 1, eapol_pkt_lens[i], f);
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

        if(eapol_pkt_lens[num] == 0)
        {
            eapol_pkt_lens[num] = len;
            memcpy(eapol_buffer + EAPOL_MAX_PKT_LEN*num, p, len);
            ++eapol_pkts_captured;
        }
        else
        {
            ESP_LOGI(TAG, "Recieved a duplicate eapol pkt %d - possibly observed partial handshake", num+1);
        }

        if(eapol_pkts_captured == EAPOL_NUM_PKTS && eapol_pkts_written_out == 0)
        {
            eapol_dump_to_disk();
        }

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

    if(client_sta_is_conn())
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
                    .required = true,
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