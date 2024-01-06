/**
 * @file wsl_bypasser.h
 * @author risinek (risinek@gmail.com)
 * @date 2021-04-05
 * @copyright Copyright (c) 2021
 * 
 * @brief Provides interface for Wi-Fi Stack Libraries bypasser
 * 
 * This component bypasses blocking mechanism that doesn't allow sending some arbitrary 802.11 frames like deauth etc.
 */
#ifndef WSL_BYPASSER_H
#define WSL_BYPASSER_H

#include <stdint.h>

/**
 * @brief Sends frame in frame_buffer using esp_wifi_80211_tx but bypasses blocking mechanism
 * 
 * @param frame_buffer 
 * @param size size of frame buffer
 *
 * @returns ESP_OK on success or else error from esp_wifi_80211_tx
 */
esp_err_t wsl_bypasser_send_raw_frame(const uint8_t *frame_buffer, int size);

/**
 * @brief Sends deauthentication frame with forged source AP from given ap_record
 *  
 * This will send deauthentication frame acting as frame from given AP, and destination will be broadcast
 * MAC address - \c ff:ff:ff:ff:ff:ff
 * 
 * @param ap_mac 6 byte MAC addr pointer to AP address
 *
 * @returns ESP_OK on success or else error from esp_wifi_80211_tx
 */
esp_err_t wsl_bypasser_send_deauth_frame(uint8_t* ap_mac);


/**
 * @brief Sends deauthentication frame with forged source AP from given ap_record
 *  
 * This will send deauthentication frame acting as frame from given AP, and destination will be broadcast
 * MAC address of that provided
 * 
 * @param ap_mac 6 byte MAC addr pointer to AP address
 * @param sta_mac 6 byte MAC addr pointer to STA address 
 *
 * @returns ESP_OK on success or else error from esp_wifi_80211_tx
 */
esp_err_t wsl_bypasser_send_deauth_frame_targted(uint8_t* ap_mac, uint8_t* sta_mac);


#endif