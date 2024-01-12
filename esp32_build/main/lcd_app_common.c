#include "lcd_app_common.h"
#include "user_interface.h"
#include "mac_logger.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"

#define SCAN_TIME_ON_EACH_CHANNEL_MS 3000


void lcd_scan_n_dump(void)
{
    esp_err_t e = ESP_OK;
    e |= ui_lock_cursor();
    e |= ui_home_screen_pos();
    e |= mac_logger_init(NULL);
    e |= ui_push_to_line_buffer(0, "Scanning Channel 1");
    e |= ui_push_to_line_buffer(1, "");
    e |= ui_push_to_line_buffer(2, "");
    e |= ui_push_to_line_buffer(3, "");
    e |= ui_update_display();
    ESP_ERROR_CHECK_WITHOUT_ABORT(e);

    // This locks up the UI task for 3s but oh well
    e = ESP_OK;
    e |= pkt_sniffer_launch(1, pkt_sniffer_filt);
    vTaskDelay(SCAN_TIME_ON_EACH_CHANNEL_MS / portTICK_PERIOD_MS);
    e |= pkt_sniffer_kill();
    e |= ui_push_to_line_buffer(0, "Scanning Channel 6");
    e |= ui_update_display();
    e |= pkt_sniffer_launch(6, pkt_sniffer_filt);
    vTaskDelay(SCAN_TIME_ON_EACH_CHANNEL_MS / portTICK_PERIOD_MS);
    e |= pkt_sniffer_kill();
    e |= ui_push_to_line_buffer(0, "Scanning Channel 11");
    e |= ui_update_display();
    e |= pkt_sniffer_launch(11, pkt_sniffer_filt);
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
