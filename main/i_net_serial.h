/**
 * @file i_net_serial.h
 * @brief UART Serial multiplayer networking for Doom
 * @note Uses Grove port (G1/G2) with software crossover
 *       NO WiFi - keeps ~240KB heap free for Doom
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Multiplayer state (shared with main.cpp) - use int for C compatibility
extern int is_multiplayer;
extern int is_master;

// Frame protocol constants
#define SERIAL_FRAME_SYNC    0x55
#define SERIAL_HANDSHAKE_M   0x5A  // Master sends this
#define SERIAL_HANDSHAKE_S   0xA5  // Slave replies with this
#define SERIAL_MAX_PAYLOAD   250
#define SERIAL_RX_BUFFER     1024

// Network packet types for multiplayer sync
#define NET_PKT_TICCMD       0x01  // Ticcmd exchange
#define NET_PKT_SYNC         0x02  // Sync/heartbeat
#define NET_PKT_ACK          0x03  // Acknowledgment
#define NET_PKT_MAP_CHANGE   0x04  // Map change (sent by master when selecting map)
#define NET_PKT_LEVEL_READY  0x05  // Level load sync handshake
#define NET_PKT_CHKSUM       0x06  // Position checksum for desync detection
#define NET_PKT_START_GAME   0x07  // Master tells slave to start game (skill/ep/map)
#define NET_PKT_ACK_START    0x08  // Slave acknowledges START_GAME
#define NET_PKT_WAITING_FOR_START 0x0A  // Master finished loading, waiting for slave
#define NET_PKT_READY_TO_START    0x0B  // Slave ready to start
#define NET_PKT_GO                0x0C  // Master tells slave to go
#define NET_PKT_EXTENDED_CHKSUM   0x0D  // Extended checksum with full player state
#define NET_PKT_STARTUP_READY     0x0E  // Both devices ready to enter doom_main

/**
 * @brief Initialize UART for multiplayer
 * Must be called AFTER role selection (is_master must be set)
 * Uses software pin crossover based on role
 * @return ESP_OK on success
 */
esp_err_t serial_net_init(void);

/**
 * @brief Perform master/slave handshake over UART
 * Master: Sends 0x5A, waits for 0xA5
 * Slave: Waits for 0x5A, sends 0xA5
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK when synced, ESP_ERR_TIMEOUT on timeout
 */
esp_err_t serial_net_handshake(int timeout_ms);

/**
 * @brief Send a framed packet over UART
 * Frame format: [0x55][Len][Data...][Checksum]
 * @param data Data buffer
 * @param len Data length (max 250 bytes)
 * @return ESP_OK on success
 */
esp_err_t serial_net_send(const uint8_t *data, size_t len);

/**
 * @brief Receive a framed packet from UART
 * @param data Buffer to store received data
 * @param max_len Buffer size
 * @param out_len Actual received length
 * @return ESP_OK if packet received, ESP_ERR_NOT_FOUND if no data
 */
esp_err_t serial_net_receive(uint8_t *data, size_t max_len, size_t *out_len);

/**
 * @brief Check if serial multiplayer is ready
 * @return true if handshake completed
 */
bool serial_net_is_ready(void);

/**
 * @brief Get free heap size (for diagnostics)
 * @return Free heap in bytes
 */
uint32_t serial_net_get_free_heap(void);

/**
 * @brief Deinitialize UART (cleanup)
 */
void serial_net_deinit(void);

// ============================================================================
// Doom Network Interface for Ticcmd Exchange
// Using Serial_ prefix to avoid collision with original Doom I_* functions
// ============================================================================

/**
 * @brief Send local player's ticcmd to remote player
 * Called each tic to transmit local input
 * @param tic Current game tic number
 * @param ticcmd_data Pointer to ticcmd_t structure (8 bytes)
 * @return ESP_OK on success
 */
esp_err_t Serial_NetSend(int tic, const void *ticcmd_data);

/**
 * @brief Receive remote player's ticcmd (non-blocking)
 * @param out_tic Receives the tic number from remote
 * @param ticcmd_data Buffer to store received ticcmd (8 bytes)
 * @return ESP_OK if packet received, ESP_ERR_NOT_FOUND if no data
 */
esp_err_t Serial_GetPacket(int *out_tic, void *ticcmd_data);

/**
 * @brief Blocking wait for remote ticcmd with timeout
 * Used for lock-step synchronization
 * @param expected_tic The tic number we're waiting for
 * @param ticcmd_data Buffer to store received ticcmd
 * @param timeout_ms Maximum time to wait
 * @return ESP_OK if received, ESP_ERR_TIMEOUT if timed out
 */
esp_err_t Serial_WaitForTiccmd(int expected_tic, void *ticcmd_data, int timeout_ms);

/**
 * @brief Get network statistics
 * @param sent_packets Pointer to receive sent count
 * @param recv_packets Pointer to receive received count  
 * @param checksum_errors Pointer to receive error count
 */
void Serial_GetNetStats(uint32_t *sent_packets, uint32_t *recv_packets, uint32_t *checksum_errors);

/**
 * @brief Suppress or restore ESP_LOG output during gameplay
 * Call with true when game starts to avoid UART1 interference
 * Call with false when returning to menu
 * @param suppress true to suppress, false to restore
 */
void serial_net_suppress_logging(bool suppress);

/**
 * @brief Send map change packet (master only)
 * Called when master selects a new map to start
 * @param episode Episode number
 * @param map Map number
 * @param skill Skill level
 * @return ESP_OK on success
 */
esp_err_t Serial_SendMapChange(int episode, int map, int skill);

/**
 * @brief Check for incoming map change packet (slave only)
 * Called by slave to see if master selected a new map
 * @param out_episode Receives episode number
 * @param out_map Receives map number
 * @param out_skill Receives skill level
 * @return ESP_OK if packet received, ESP_ERR_NOT_FOUND if no data
 */
esp_err_t Serial_CheckMapChange(int *out_episode, int *out_map, int *out_skill);

/**
 * @brief Flush all UART and ticcmd buffers before starting a new game
 * Prevents "ghost inputs" from menu navigation affecting gameplay
 * Call this after map change before starting gameplay
 */
void Serial_FlushBuffers(void);

/**
 * @brief Get number of detected desync events
 * @return Count of desyncs since last flush
 */
int Serial_GetDesyncCount(void);

/**
 * @brief Get the last tic number received from remote player
 * @return Last remote tic, or -1 if none received yet
 */
int Serial_GetLastRemoteTic(void);

/**
 * @brief Send PKT_LEVEL_READY and wait for remote PKT_LEVEL_READY
 * BLOCKING: Both devices must complete this before first P_Ticker
 * @param timeout_ms Maximum wait time (use 5000+ for safety)
 * @return ESP_OK if both sides synced, ESP_ERR_TIMEOUT on failure
 */
esp_err_t Serial_LevelReadySync(int timeout_ms);

/**
 * @brief Startup sync before entering doom_main (prevents race on boot)
 * Both devices must acknowledge they are ready before Doom starts
 * @param timeout_ms Maximum wait time
 * @return ESP_OK if both sides synced, ESP_ERR_TIMEOUT on failure
 */
esp_err_t Serial_StartupSync(int timeout_ms);

/**
 * @brief Master sends START_GAME packet to tell slave to begin
 * Called when master selects New Game from menu
 * BLOCKING: Sends repeatedly until ACK_START received or timeout
 * @param skill Skill level (0-4)
 * @param episode Episode number (1-4)
 * @param map Map number (1-9)
 * @param timeout_ms Maximum time to wait for ACK
 * @return ESP_OK if slave acknowledged, ESP_ERR_TIMEOUT if no response
 */
esp_err_t Serial_StartGameHandshake(int skill, int episode, int map, int timeout_ms);

/**
 * @brief Slave polls for START_GAME packet from master
 * Call this in menu loop to check if master started a game
 * @param out_skill Receives skill level
 * @param out_episode Receives episode number
 * @param out_map Receives map number
 * @return ESP_OK if packet received, ESP_ERR_NOT_FOUND if no data
 */
esp_err_t Serial_CheckStartGame(int *out_skill, int *out_episode, int *out_map);

/**
 * @brief Slave sends ACK_START to master after receiving START_GAME
 * @return ESP_OK on success
 */
esp_err_t Serial_SendStartAck(void);

/**
 * @brief Send position checksum packet (Master only, every 64 tics)
 * @param tic Current game tic
 * @param player0_x Player 0 X position (fixed_t)
 * @param player0_y Player 0 Y position (fixed_t)
 * @return ESP_OK on success
 */
esp_err_t Serial_SendChecksum(int tic, int player0_x, int player0_y);

/**
 * @brief Check for incoming checksum packet (Slave only)
 * @param out_tic Receives the tic number
 * @param out_x Receives player 0 X position
 * @param out_y Receives player 0 Y position
 * @return ESP_OK if packet received, ESP_ERR_NOT_FOUND if no data
 */
esp_err_t Serial_CheckChecksum(int *out_tic, int *out_x, int *out_y);

/**
 * @brief Send extended checksum packet with full player state (Master only, every 16 tics)
 * Includes both players' positions, angles, and health for zero-tolerance sync
 * @param tic Current game tic
 * @param p0_x Player 0 X position
 * @param p0_y Player 0 Y position
 * @param p0_angle Player 0 angle
 * @param p0_health Player 0 health
 * @param p1_x Player 1 X position
 * @param p1_y Player 1 Y position
 * @param p1_angle Player 1 angle
 * @param p1_health Player 1 health
 * @return ESP_OK on success
 */
esp_err_t Serial_SendExtendedChecksum(int tic, int episode, int map,
    int p0_x, int p0_y, int p0_angle, int p0_health,
    int p1_x, int p1_y, int p1_angle, int p1_health);

/**
 * @brief Check for incoming extended checksum packet (Slave only)
 * @param out_tic Receives the tic number
 * @param out_episode Receives the episode number (1-based)
 * @param out_map Receives the map number (1-based)
 * @param out_p0_x Receives player 0 X position
 * @param out_p0_y Receives player 0 Y position
 * @param out_p0_angle Receives player 0 angle
 * @param out_p0_health Receives player 0 health
 * @param out_p1_x Receives player 1 X position
 * @param out_p1_y Receives player 1 Y position
 * @param out_p1_angle Receives player 1 angle
 * @param out_p1_health Receives player 1 health
 * @return ESP_OK if packet received, ESP_ERR_NOT_FOUND if no data
 */
esp_err_t Serial_CheckExtendedChecksum(int *out_tic, int *out_episode, int *out_map,
    int *out_p0_x, int *out_p0_y, int *out_p0_angle, int *out_p0_health,
    int *out_p1_x, int *out_p1_y, int *out_p1_angle, int *out_p1_health);

/**
 * @brief Check for incoming handshake packets (WAITING_FOR_START, READY_TO_START, GO)
 * Used during gameplay to detect if master has started a new level
 * @return ESP_OK if handshake packets detected, ESP_ERR_NOT_FOUND if none
 */
esp_err_t Serial_CheckHandshakePackets(void);

/**
 * @brief Perform 3-way handshake for level loading synchronization
 * Master: Send PKT_WAITING_FOR_START, wait for PKT_READY_TO_START, send PKT_GO
 * Slave: Wait for PKT_WAITING_FOR_START, send PKT_READY_TO_START, wait for PKT_GO
 * @param timeout_ms Timeout for each step in milliseconds
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on failure
 */
esp_err_t Serial_LevelLoad3WayHandshake(int timeout_ms);

/**
 * @brief Global desync flag - set true to halt game (use int for C compatibility)
 */
extern int desync_detected;

/**
 * @brief Flag to skip 3-way handshake during level load (for mismatch correction)
 * Set to 1 before triggering ga_loadlevel when slave detects level mismatch
 */
extern int skip_level_handshake;

/**
 * @brief Check if master is on a different level than slave (slave only)
 * Call this frequently from game loop to detect level mismatch
 * @param current_episode Current episode number (1-based)
 * @param current_map Current map number (1-based)
 * @param out_target_episode Receives target episode if mismatch
 * @param out_target_map Receives target map if mismatch
 * @return ESP_OK if mismatch detected, ESP_ERR_NOT_FOUND if no mismatch
 */
esp_err_t Serial_CheckLevelMismatch(int current_episode, int current_map,
                                     int *out_target_episode, int *out_target_map);

/**
 * @brief Update current level (call from game code when level changes)
 * @param episode Current episode (1-based)
 * @param map Current map (1-based)
 */
void Serial_UpdateCurrentLevel(int episode, int map);

/**
 * @brief Clear level mismatch flag (call after level load completes)
 */
void Serial_ClearLevelMismatch(void);

#ifdef __cplusplus
}
#endif
