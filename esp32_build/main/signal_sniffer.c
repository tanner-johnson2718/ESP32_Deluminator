// init         -> Do an initial scan of channels 1, 6, and 11 for a preset
//                 time, filling the mac logger with STAs and APs. Stop the
//                 sniffer, present all the found AP/MACS and wait for the
//                 user to select one.
//
// first press  -> see list of STA and which ones are APs
// second press -> Print to screen the updated RSSI of 

#include "user_interface.h"
#include "mac_logger.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"

#define TIMER_DELAY_MS 1000
#define SCAN_TIME_ON_EACH_CHANNEL_MS 1000

/*
static esp_timer_handle_t gp_timer;
static esp_timer_create_args_t gp_timer_args = 
{
    .callback = NULL,
    .name = "Wifi GP Timer"
};
static uint8_t gp_timer_running = 0;
*/

void lcd_signal_sniffer_init(void)
{

    esp_err_t e = ESP_OK;
    e |= ui_lock_cursor();
    e |= ui_home_screen_pos();
    e |= mac_logger_init();
    e |= ui_push_to_line_buffer(0, "Scanning 3");
    e |= ui_push_to_line_buffer(1, "");
    e |= ui_push_to_line_buffer(2, "");
    e |= ui_push_to_line_buffer(3, "");
    e |= ui_update_display();
    ESP_ERROR_CHECK_WITHOUT_ABORT(e);
    
    wifi_promiscuous_filter_t filt = 
    {
        .filter_mask=WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
    };

    // This locks up the UI task for 3s but oh well
    e = ESP_OK;
    e |= pkt_sniffer_launch(1, filt);
    vTaskDelay(SCAN_TIME_ON_EACH_CHANNEL_MS / portTICK_PERIOD_MS);
    e |= pkt_sniffer_kill();
    e |= ui_push_to_line_buffer(0, "Scanning 2");
    e |= ui_update_display();
    e |= pkt_sniffer_launch(6, filt);
    vTaskDelay(SCAN_TIME_ON_EACH_CHANNEL_MS / portTICK_PERIOD_MS);
    e |= pkt_sniffer_kill();
    e |= ui_push_to_line_buffer(0, "Scanning 1");
    e |= ui_update_display();
    e |= pkt_sniffer_launch(11, filt);
    vTaskDelay(SCAN_TIME_ON_EACH_CHANNEL_MS / portTICK_PERIOD_MS);
    e |= pkt_sniffer_kill();
    ESP_ERROR_CHECK_WITHOUT_ABORT(e);

    int8_t i, n;
    char line[20];
    sta_t sta;
    ap_t ap;
    ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_ap_list_len(&n));
    e = ESP_OK;
    for(i = 0; i < n; ++i)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_ap(i, &sta, &ap));
        snprintf(line, 20, "%.19s", ap.ssid);

        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(i, line));
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_unlock_cursor());
}

void lcd_signal_sniffer_cb(uint8_t index)
{

}

void lcd_signal_sniffer_fini(void)
{

}