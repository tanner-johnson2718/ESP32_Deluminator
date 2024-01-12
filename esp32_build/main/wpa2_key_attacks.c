// TODO Doc on Deauth and EAPOL and the general auth, assoc connection state
// machine.

#include "lcd_app_common.h"
#include <dirent.h>
#include <string.h>
#include "user_interface.h"
#include "mac_logger.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "eapol_logger.h"


#define MOUNT_PATH "/spiffs"

static esp_timer_handle_t gp_timer;
static esp_timer_create_args_t gp_timer_args = 
{
    .callback = NULL,
    .name = "LCD WPA2 GP Timer"
};
static uint8_t gp_timer_running = 0;
static int8_t target_ap = -1;

//*****************************************************************************
// WPA2 Passive Attack - Simply listen for EAPOL handshakes on a single AP. 
// They will be auto saved to the flash memory when a full handshake is oberved
//****************************************************************************

void lcd_wpa2_passive_pwn_init(void)
{
    lcd_scan_n_dump();
}

void lcd_wpa2_passive_pwn_fini(void)
{
    esp_err_t e = ESP_OK;
    if(gp_timer_running)
    {
        e |= esp_timer_stop(gp_timer);
        e |= esp_timer_delete(gp_timer);
        gp_timer_running = 0;    
    }

    target_ap = -1;
    e |= pkt_sniffer_kill();
    e |= mac_logger_clear();
    e |= pkt_sniffer_clear_filter_list();

    ESP_ERROR_CHECK_WITHOUT_ABORT(e);
}

void passive_timer_func(void* args)
{
    DIR *d;
    struct dirent *dir;
    uint8_t i;

    d = opendir(MOUNT_PATH);
    if(d)
    {
        while((dir = readdir(d))!=NULL)
        {
            i = 0;
            while(dir->d_name[i] != 0)
            {
                if(dir->d_name[i] == '.')
                {
                    if(strcmp(dir->d_name + i, ".pkt") == 0)
                    {
                        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(3, "HANDSHAKE CAPTURED"));
                        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
                        lcd_wpa2_passive_pwn_fini();
                        return;
                    }
                }

                ++i;
            }
            
        }
        closedir(d);
    }
}

void lcd_wpa2_passive_pwn_cb(uint8_t index)
{
    int16_t n;
    ap_t ap;
    sta_t sta;
    esp_err_t e = ESP_OK;
    char line[20];

    if(gp_timer_running){ return; }

    ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_sta_list_len(&n));
    if(index >= n)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
        return;
    }

    target_ap = index;
    e |= mac_logger_get_ap(index, &sta, &ap);
    e |= mac_logger_clear();
    e |= pkt_sniffer_clear_filter_list();

    e |= ui_lock_cursor();
    e |= ui_home_screen_pos();
    snprintf(line, 20, "%.19s", ap.ssid);
    e |= ui_push_to_line_buffer(0, line);
    snprintf(line, 20, MACSTR, MAC2STR(sta.mac));
    e |= ui_push_to_line_buffer(1, line);
    snprintf(line, 20, "Num STAs = %02d", ap.num_assoc_stas);
    e |= ui_push_to_line_buffer(2, line);
    e |= ui_push_to_line_buffer(3, "");
    e |= ui_update_display();

    e |= eapol_logger_init(sta.mac);
    e |= pkt_sniffer_launch(ap.channel, pkt_sniffer_filt);

    gp_timer_args.callback = passive_timer_func;
    
    e |= esp_timer_create(&gp_timer_args, &gp_timer);
    e |= esp_timer_start_periodic(gp_timer, TIMER_DELAY_MS*1000);
    
    gp_timer_running = 1;
    ESP_ERROR_CHECK_WITHOUT_ABORT(e);
}
//*****************************************************************************
// WPA2 Targeted Attack - This is a manual attack where a specific MAC address
// is targeted and upon the users click, a deauth is sent. Multiple can be 
// sent until the handshake is captured
//*****************************************************************************


//*****************************************************************************
// WPA2 Agro Attacl - In this attack, on timer, for every AP we send out a 
// broadcast deauth mesage from that AP trying to collect as much as possible
//*****************************************************************************/ 