// init         -> Do an initial scan of channels 1, 6, and 11 for a preset
//                 time, filling the mac logger with STAs and APs. Stop the
//                 sniffer, present all the found APs found with num stas.
//
// first press  -> Seeing the list of APs, on first click, we clear the mac
//                 logger and pkt sniffer filters. We add an AP filter to the 
//                 mac logger so only MACs sending traffic to and from that AP 
//                 are visable. Then we display the rssi of all macs on that AP
//
// nth press -> Does nothing as we a long press can be used to reset.

#include "lcd_app_common.h"
#include "user_interface.h"
#include "mac_logger.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define TIMER_DELAY_MS 1000

void timer_func(void* args);
static esp_timer_handle_t gp_timer;
static esp_timer_create_args_t gp_timer_args = 
{
    .callback = timer_func,
    .name = "Wifi GP Timer"
};
static uint8_t gp_timer_running = 0;
static int8_t target_ap = -1;

void lcd_signal_sniffer_init(void)
{
    lcd_scan_n_dump();
}

void timer_func(void* args)
{

    int16_t i, n;
    char line[20];
    sta_t sta;
    ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_sta_list_len(&n));

    for(i = 0; i < n; ++i)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_sta(i, &sta));
        snprintf(line, 20, MACSTR, MAC2STR(sta.mac));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(2*i, line));
        snprintf(line, 20, "RSSI = %03d", sta.rssi);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer((2*i) +1, line));
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
}

void lcd_signal_sniffer_cb(uint8_t index)
{
    int16_t n;
    ap_t ap;
    sta_t sta;
    esp_err_t e;

    if(gp_timer_running)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_sta_list_len(&n));
    if(index >= n)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
        return;
    }

    target_ap = index;

    e = ESP_OK;
    e |= ui_lock_cursor();
    e |= ui_home_screen_pos();
    e |= ui_push_to_line_buffer(0, "");
    e |= ui_push_to_line_buffer(1, "");
    e |= ui_push_to_line_buffer(2, "");
    e |= ui_push_to_line_buffer(3, "");
    e |= ui_update_display();
    e |= mac_logger_get_ap(index, &sta, &ap);
    e |= mac_logger_clear();
    e |= pkt_sniffer_clear_filter_list();
    e |= mac_logger_init(sta.mac);
    e |= pkt_sniffer_launch(ap.channel, pkt_sniffer_filt);
    e |= esp_timer_create(&gp_timer_args, &gp_timer);
    e |= esp_timer_start_periodic(gp_timer, TIMER_DELAY_MS*1000);
    e |= ui_unlock_cursor();
    gp_timer_running = 1;
    ESP_ERROR_CHECK_WITHOUT_ABORT(e);

}

void lcd_signal_sniffer_fini(void)
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