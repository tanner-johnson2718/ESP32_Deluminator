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

#include "user_interface.h"
#include "mac_logger.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"

#define TIMER_DELAY_MS 1000
#define SCAN_TIME_ON_EACH_CHANNEL_MS 3000

static const char* TAG = "LCD Signal Sniffer";

void timer_func(void* args);
static esp_timer_handle_t gp_timer;
static esp_timer_create_args_t gp_timer_args = 
{
    .callback = timer_func,
    .name = "Wifi GP Timer"
};
static uint8_t gp_timer_running = 0;


void lcd_signal_sniffer_init(void)
{

    esp_err_t e = ESP_OK;
    e |= ui_lock_cursor();
    e |= ui_home_screen_pos();
    e |= mac_logger_init(NULL);
    e |= ui_push_to_line_buffer(0, "Scanning 3");
    e |= ui_push_to_line_buffer(1, "");
    e |= ui_push_to_line_buffer(2, "");
    e |= ui_push_to_line_buffer(3, "");
    e |= ui_update_display();
    ESP_ERROR_CHECK_WITHOUT_ABORT(e);

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
        snprintf(line, 20, "%.15s %02d", ap.ssid, ap.num_assoc_stas);

        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_push_to_line_buffer(i, line));
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui_unlock_cursor());
}

void timer_func(void* args)
{

}

void lcd_signal_sniffer_cb(uint8_t index)
{
    int8_t n;
    ap_t ap;
    sta_t sta;
    esp_err_t e;
    wifi_promiscuous_filter_t filt = 
    {
        .filter_mask=WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
    };

    if(gp_timer_running)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(mac_logger_get_sta_list_len(&n));
    if(index >= n)
    {
        ESP_LOGE(TAG, "tried to access AP beyond list len");
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_home_screen_pos());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui_update_display());
        return;
    }

    e = ESP_OK;
    e |= ui_lock_cursor();
    e |= ui_home_screen_pos();
    e |= ui_push_to_line_buffer(0, "");
    e |= ui_push_to_line_buffer(1, "");
    e |= ui_push_to_line_buffer(2, "");
    e |= ui_push_to_line_buffer(3, "");
    e |= ui_update_display();
    e |= mac_logger_clear();
    e |= pkt_sniffer_clear_filter_list();
    e |= mac_logger_get_ap(index, &sta, &ap);
    e |= mac_logger_init(sta.mac);
    e |= pkt_sniffer_launch(ap.channel, filt);
    e |= esp_timer_create(&gp_timer_args, &gp_timer);
    e |= esp_timer_start_periodic(gp_timer, TIMER_DELAY_MS*1000);
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

    e |= pkt_sniffer_kill();
    e |= mac_logger_clear();
    e |= pkt_sniffer_clear_filter_list();

    ESP_ERROR_CHECK_WITHOUT_ABORT(e);
}