/**
 * @file i_net_espnow.h
 * @brief ESP-NOW multiplayer networking for Doom
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "esp_err.h"

// Multiplayer state - defined in i_net_serial.c, use int for C compatibility
extern int is_multiplayer;
extern int is_master;

/**
 * @brief Initialize ESP-NOW networking
 * @param master True if this device is master, false for slave
 * @return ESP_OK on success
 */
esp_err_t espnow_init(bool master);

/**
 * @brief Perform master/slave pairing handshake
 * @return ESP_OK when connected
 */
esp_err_t espnow_pair(void);

/**
 * @brief Send game data packet
 * @param data Data buffer
 * @param len Data length
 * @return ESP_OK on success
 */
esp_err_t espnow_send(const uint8_t *data, size_t len);

/**
 * @brief Receive game data packet
 * @param data Buffer to store received data
 * @param len Buffer size / Returns actual received size
 * @return ESP_OK if data received, ESP_ERR_NOT_FOUND if no data
 */
esp_err_t espnow_receive(uint8_t *data, size_t *len);

/**
 * @brief Check if paired and ready
 * @return true if ready for multiplayer
 */
bool espnow_is_ready(void);

/**
 * @brief Get free heap size
 * @return Free heap in bytes
 */
uint32_t espnow_get_free_heap(void);

/**
 * @brief Suspend ESP-NOW callbacks to prevent interference during heavy init
 * Call this before doom_main() to avoid crash during Z_Init/I_InitSound
 */
void espnow_suspend_callbacks(void);

/**
 * @brief Resume ESP-NOW callbacks after Doom initialization is complete
 * Call this once the game reaches the title screen or game loop
 */
void espnow_resume_callbacks(void);

/**
 * @brief Clear any pending messages in the receive queue
 */
void espnow_flush_queue(void);

/**
 * @brief Fully deinitialize WiFi/ESP-NOW to free ~54KB RAM
 * Saves peer MAC for later reconnection. Call after successful pairing
 * but before doom_main() to ensure enough memory for level loading.
 */
void espnow_deinit_wifi(void);

/**
 * @brief Reinitialize WiFi/ESP-NOW and restore peer connection
 * Call this when multiplayer sync is actually needed during gameplay.
 * @return ESP_OK on success, ESP_FAIL if reinit fails
 */
esp_err_t espnow_reinit_wifi(void);

/**
 * @brief Check if WiFi is currently deinitialized
 * @return true if WiFi is down (was deinitialized to save memory)
 */
bool espnow_is_wifi_down(void);

#ifdef __cplusplus
}
#endif
