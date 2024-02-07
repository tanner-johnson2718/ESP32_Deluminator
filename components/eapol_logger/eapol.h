#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t eapol_logger_send_raw_frame(const uint8_t *frame_buffer, int size);
esp_err_t eapol_logger_send_deauth_frame(uint8_t* ap_mac);
esp_err_t eapol_logger_send_deauth_frame_targted(uint8_t* ap_mac, uint8_t* sta_mac);

esp_err_t eapol_logger_init(uint8_t mac_logger_ap_index);
esp_err_t eapol_logger_clear(void);