/**
 * @file i_net_serial.c
 * @brief UART Serial multiplayer networking implementation
 * @note Grove port with software crossover - NO WiFi overhead
 */
#include "i_net_serial.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#define SERIAL_TAG "SERIAL_NET"
#define UART_PORT  UART_NUM_1
#define BAUD_RATE  256000
#define GROVE_G1   1
#define GROVE_G2   2
// Flag to suppress verbose logging during gameplay to avoid UART interference
static bool gameplay_log_suppressed = false;
// Multiplayer state - defined as int for C compatibility with Doom code
int is_multiplayer = 0;
int is_master = 0;
static bool serial_initialized = false;
static bool handshake_complete = false;
// RX ring buffer for incoming data
static uint8_t rx_ring_buffer[SERIAL_RX_BUFFER];
static size_t rx_head = 0;
static size_t rx_tail = 0;
// Pending START_GAME packet (so it doesn't get lost when other packets arrive)
static bool pending_start_game = false;
static int pending_start_skill = 0;
static int pending_start_episode = 0;
static int pending_start_map = 0;
// Pending ACK_START (for master's handshake)
static bool pending_ack_start = false;
// Current level for slave - updated by game code
static int current_episode = 0;
static int current_map = 0;
// Level mismatch detection - set when EXTENDED_CHKSUM shows different level
static bool level_mismatch_detected = false;
static int mismatch_target_episode = 0;
static int mismatch_target_map = 0;
// Flag to skip 3-way handshake during level load (for mismatch correction)
int skip_level_handshake = 0;
// Pending 3-way handshake packets for level loading
static bool pending_waiting_for_start = false;
static bool pending_ready_to_start = false;
static bool pending_go = false;
// Pending extended checksum for zero-tolerance player state sync
static bool pending_extended_checksum = false;
static int pending_extended_tic = 0;
static int pending_extended_episode = 0;  // Current episode (1-based)
static int pending_extended_map = 0;      // Current map (1-based)
static int pending_extended_p0_x = 0;
static int pending_extended_p0_y = 0;
static int pending_extended_p0_angle = 0;
static int pending_extended_p0_health = 0;
static int pending_extended_p1_x = 0;
static int pending_extended_p1_y = 0;
static int pending_extended_p1_angle = 0;
static int pending_extended_p1_health = 0;
// Slave level tracking (for master to detect slave's level)
static bool pending_slave_level = false;
static int pending_slave_episode = 0;
static int pending_slave_map = 0;
// Frame parsing state
typedef enum {
    FRAME_WAIT_SYNC,
    FRAME_WAIT_LEN,
    FRAME_WAIT_DATA,
    FRAME_WAIT_CHECKSUM
} frame_state_t;
static frame_state_t frame_state = FRAME_WAIT_SYNC;
static uint8_t frame_buffer[SERIAL_MAX_PAYLOAD + 4];
static size_t frame_len = 0;
static size_t frame_pos = 0;
esp_err_t serial_net_init(void)
{
    if (serial_initialized) {
        ESP_LOGW(SERIAL_TAG, "Already initialized");
        return ESP_OK;
    }
    ESP_LOGI(SERIAL_TAG, "Initializing UART (role: %s)", is_master ? "MASTER" : "SLAVE");
    ESP_LOGI(SERIAL_TAG, "Free heap before UART init: %lu bytes", esp_get_free_heap_size());
    // UART configuration
    uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // Install UART driver with minimal buffer
    esp_err_t ret = uart_driver_install(UART_PORT, SERIAL_RX_BUFFER, 256, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(SERIAL_TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = uart_param_config(UART_PORT, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(SERIAL_TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    // SOFTWARE CROSSOVER based on role
    // Grove cable is straight, so we swap TX/RX based on who we are
    int tx_pin, rx_pin;
    if (is_master) {
        // Master: TX=G1, RX=G2
        tx_pin = GROVE_G1;
        rx_pin = GROVE_G2;
    } else {
        // Slave: TX=G2, RX=G1 (crossed)
        tx_pin = GROVE_G2;
        rx_pin = GROVE_G1;
    }
    ESP_LOGI(SERIAL_TAG, "Pin config: TX=GPIO%d, RX=GPIO%d", tx_pin, rx_pin);
    ret = uart_set_pin(UART_PORT, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(SERIAL_TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }
    // Flush any garbage
    uart_flush(UART_PORT);
    serial_initialized = true;
    ESP_LOGI(SERIAL_TAG, "UART initialized at %d baud", BAUD_RATE);
    ESP_LOGI(SERIAL_TAG, "Free heap after UART init: %lu bytes", esp_get_free_heap_size());
    return ESP_OK;
}
esp_err_t serial_net_handshake(int timeout_ms)
{
    if (!serial_initialized) {
        ESP_LOGE(SERIAL_TAG, "Not initialized");
        return ESP_FAIL;
    }
    ESP_LOGI(SERIAL_TAG, "Starting handshake (timeout: %dms)...", timeout_ms);
    // Flush any stale data from UART buffer
    uart_flush(UART_PORT);
    uint8_t tx_byte = is_master ? SERIAL_HANDSHAKE_M : SERIAL_HANDSHAKE_S;
    uint8_t expect_byte = is_master ? SERIAL_HANDSHAKE_S : SERIAL_HANDSHAKE_M;
    uint8_t rx_byte;
    int elapsed = 0;
    const int interval = 200;  // Increased to 200ms for better synchronization window
    while (elapsed < timeout_ms) {
        // Send our handshake byte
        uart_write_bytes(UART_PORT, (const char*)&tx_byte, 1);
        // Check for response
        int len = uart_read_bytes(UART_PORT, &rx_byte, 1, pdMS_TO_TICKS(interval));
        if (len > 0) {
            ESP_LOGI(SERIAL_TAG, "Received byte: 0x%02X (expecting 0x%02X)", rx_byte, expect_byte);
            if (rx_byte == expect_byte) {
                // Got the expected handshake!
                if (is_master) {
                    // Master received 0xA5 from slave - we're synced!
                    ESP_LOGI(SERIAL_TAG, "HANDSHAKE SUCCESS (Master got slave ack)");
                } else {
                    // Slave received 0x5A from master, we already sent 0xA5
                    // Send one more 0xA5 to confirm
                    uart_write_bytes(UART_PORT, (const char*)&tx_byte, 1);
                    ESP_LOGI(SERIAL_TAG, "HANDSHAKE SUCCESS (Slave synced with master)");
                }
                handshake_complete = true;
                // DO NOT flush UART after handshake - it might discard the first ticcmd
                // vTaskDelay(pdMS_TO_TICKS(50));
                // uart_flush(UART_PORT);
                return ESP_OK;
            }
        }
        elapsed += interval;
    }
    ESP_LOGE(SERIAL_TAG, "Handshake TIMEOUT after %dms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}
static uint8_t calculate_checksum(const uint8_t *data, size_t len)
{
    // XOR checksum
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}
esp_err_t serial_net_send(const uint8_t *data, size_t len)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    if (len > SERIAL_MAX_PAYLOAD) {
        ESP_LOGE(SERIAL_TAG, "Payload too large: %d > %d", len, SERIAL_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }
    // Build frame: [SYNC][LEN][DATA...][CHECKSUM]
    uint8_t frame[SERIAL_MAX_PAYLOAD + 3];
    frame[0] = SERIAL_FRAME_SYNC;
    frame[1] = (uint8_t)len;
    memcpy(&frame[2], data, len);
    frame[2 + len] = calculate_checksum(data, len);
    // Send frame
    int written = uart_write_bytes(UART_PORT, (const char*)frame, 3 + len);
    if (written != (int)(3 + len)) {
        ESP_LOGE(SERIAL_TAG, "Write failed: %d/%d bytes", written, 3 + len);
        return ESP_FAIL;
    }
    return ESP_OK;
}
esp_err_t serial_net_receive(uint8_t *data, size_t max_len, size_t *out_len)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    // 1. Drain UART HW buffer into our software ring buffer
    size_t available = 0;
    uart_get_buffered_data_len(UART_PORT, &available);
    if (available > 0) {
        uint8_t temp[128];
        while (available > 0) {
            int to_read = (available > sizeof(temp)) ? sizeof(temp) : available;
            int len = uart_read_bytes(UART_PORT, temp, to_read, 0);
            if (len <= 0) break;
            for (int i = 0; i < len; i++) {
                rx_ring_buffer[rx_head] = temp[i];
                rx_head = (rx_head + 1) % SERIAL_RX_BUFFER;
                // Optional: Check for overflow, but 1024 is plenty for 35Hz
            }
            available -= len;
        }
    }
    // 2. Process bytes from ring buffer
    while (rx_tail != rx_head) {
        uint8_t b = rx_ring_buffer[rx_tail];
        rx_tail = (rx_tail + 1) % SERIAL_RX_BUFFER;
        switch (frame_state) {
        case FRAME_WAIT_SYNC:
            if (b == SERIAL_FRAME_SYNC) {
                frame_state = FRAME_WAIT_LEN;
                frame_pos = 0;
            }
            break;
        case FRAME_WAIT_LEN:
            if (b > 0 && b <= SERIAL_MAX_PAYLOAD) {
                frame_len = b;
                frame_state = FRAME_WAIT_DATA;
                frame_pos = 0;
            } else {
                // Invalid length, reset
                frame_state = FRAME_WAIT_SYNC;
            }
            break;
        case FRAME_WAIT_DATA:
            frame_buffer[frame_pos++] = b;
            if (frame_pos >= frame_len) {
                frame_state = FRAME_WAIT_CHECKSUM;
            }
            break;
        case FRAME_WAIT_CHECKSUM:
            {
                uint8_t expected = calculate_checksum(frame_buffer, frame_len);
                if (b == expected) {
                    // Valid frame! Check for special packet types that need buffering
                    // START_GAME packet - save for slave's M_Ticker
                    if (frame_len >= 4 && frame_buffer[0] == NET_PKT_START_GAME) {
                        pending_start_game = true;
                        pending_start_skill = frame_buffer[1];
                        pending_start_episode = frame_buffer[2];
                        pending_start_map = frame_buffer[3];
                        printf("NET RX: Captured START_GAME (skill=%d ep=%d map=%d)\n",
                               pending_start_skill, pending_start_episode, pending_start_map);
                        frame_state = FRAME_WAIT_SYNC;
                        break;  // Continue processing
                    }
                    // ACK_START packet - save for master's handshake
                    if (frame_len >= 1 && frame_buffer[0] == NET_PKT_ACK_START) {
                        pending_ack_start = true;
                        printf("NET RX: Captured ACK_START from slave\n");
                        frame_state = FRAME_WAIT_SYNC;
                        break;  // Continue processing
                    }
                    // 3-way handshake packets for level loading
                    if (frame_len >= 1 && frame_buffer[0] == NET_PKT_WAITING_FOR_START) {
                        pending_waiting_for_start = true;
                        printf("NET RX: Captured PKT_WAITING_FOR_START from master\n");
                        frame_state = FRAME_WAIT_SYNC;
                        break;
                    }
                    if (frame_len >= 1 && frame_buffer[0] == NET_PKT_READY_TO_START) {
                        pending_ready_to_start = true;
                        printf("NET RX: Captured PKT_READY_TO_START from slave\n");
                        frame_state = FRAME_WAIT_SYNC;
                        break;
                    }
                    if (frame_len >= 1 && frame_buffer[0] == NET_PKT_GO) {
                        pending_go = true;
                        printf("NET RX: Captured PKT_GO from master\n");
                        frame_state = FRAME_WAIT_SYNC;
                        break;
                    }
                    // EXTENDED_CHKSUM packet - save for zero-tolerance player state sync
                    // New format includes episode/map for level sync: 39 bytes
                    if (frame_len >= 39 && frame_buffer[0] == NET_PKT_EXTENDED_CHKSUM) {
                        // Parse the packet
                        // Format: [PKT_TYPE][TIC:4][EPISODE:1][MAP:1][P0_X:4][P0_Y:4][P0_ANGLE:4][P0_HEALTH:4][P1_X:4][P1_Y:4][P1_ANGLE:4][P1_HEALTH:4]
                        int idx = 1;
                        pending_extended_tic = frame_buffer[idx] | (frame_buffer[idx+1]<<8) | (frame_buffer[idx+2]<<16) | (frame_buffer[idx+3]<<24); idx+=4;
                        pending_extended_episode = frame_buffer[idx++];  // 1-based episode
                        pending_extended_map = frame_buffer[idx++];      // 1-based map
                        pending_extended_p0_x = frame_buffer[idx] | (frame_buffer[idx+1]<<8) | (frame_buffer[idx+2]<<16) | (frame_buffer[idx+3]<<24); idx+=4;
                        pending_extended_p0_y = frame_buffer[idx] | (frame_buffer[idx+1]<<8) | (frame_buffer[idx+2]<<16) | (frame_buffer[idx+3]<<24); idx+=4;
                        pending_extended_p0_angle = frame_buffer[idx] | (frame_buffer[idx+1]<<8) | (frame_buffer[idx+2]<<16) | (frame_buffer[idx+3]<<24); idx+=4;
                        pending_extended_p0_health = frame_buffer[idx] | (frame_buffer[idx+1]<<8) | (frame_buffer[idx+2]<<16) | (frame_buffer[idx+3]<<24); idx+=4;
                        pending_extended_p1_x = frame_buffer[idx] | (frame_buffer[idx+1]<<8) | (frame_buffer[idx+2]<<16) | (frame_buffer[idx+3]<<24); idx+=4;
                        pending_extended_p1_y = frame_buffer[idx] | (frame_buffer[idx+1]<<8) | (frame_buffer[idx+2]<<16) | (frame_buffer[idx+3]<<24); idx+=4;
                        pending_extended_p1_angle = frame_buffer[idx] | (frame_buffer[idx+1]<<8) | (frame_buffer[idx+2]<<16) | (frame_buffer[idx+3]<<24); idx+=4;
                        pending_extended_p1_health = frame_buffer[idx] | (frame_buffer[idx+1]<<8) | (frame_buffer[idx+2]<<16) | (frame_buffer[idx+3]<<24); idx+=4;
                        pending_extended_checksum = true;
                        printf("NET RX: Captured EXTENDED_CHKSUM tic=%d E%dM%d P0(%d,%d,%d,%d) P1(%d,%d,%d,%d)\n",
                               pending_extended_tic, pending_extended_episode, pending_extended_map,
                               pending_extended_p0_x, pending_extended_p0_y, pending_extended_p0_angle, pending_extended_p0_health,
                               pending_extended_p1_x, pending_extended_p1_y, pending_extended_p1_angle, pending_extended_p1_health);
                        // Immediately flag level mismatch for quick detection
                        // (The actual game level check happens in Serial_CheckLevelMismatch)
                        if (!is_master && (pending_extended_episode > 0 && pending_extended_map > 0)) {
                            // Store the target level - game code will compare against current
                            mismatch_target_episode = pending_extended_episode;
                            mismatch_target_map = pending_extended_map;
                        }
                        frame_state = FRAME_WAIT_SYNC;
                        break;
                    }
                    // SLAVE_LEVEL packet - master receives this from slave
                    if (frame_len >= 3 && frame_buffer[0] == NET_PKT_SLAVE_LEVEL) {
                        pending_slave_level = true;
                        pending_slave_episode = frame_buffer[1];
                        pending_slave_map = frame_buffer[2];
                        printf("NET RX: Captured SLAVE_LEVEL (E%dM%d)\n",
                               pending_slave_episode, pending_slave_map);
                        frame_state = FRAME_WAIT_SYNC;
                        break;
                    }
                    // Normal packet - return it
                    size_t copy_len = (frame_len <= max_len) ? frame_len : max_len;
                    memcpy(data, frame_buffer, copy_len);
                    *out_len = copy_len;
                    frame_state = FRAME_WAIT_SYNC;
                    return ESP_OK;
                } else {
                    ESP_LOGW(SERIAL_TAG, "Checksum mismatch: got 0x%02X, expected 0x%02X", 
                             b, expected);
                }
                frame_state = FRAME_WAIT_SYNC;
            }
            break;
        }
    }
    // No complete frame yet
    return ESP_ERR_NOT_FOUND;
}
// Forward declaration (defined later)
static void poll_and_queue_ticcmds(void);
/**
 * @brief Update current level (call from game code when level changes)
 * @param episode Current episode (1-based)
 * @param map Current map (1-based)
 */
void Serial_UpdateCurrentLevel(int episode, int map)
{
    current_episode = episode;
    current_map = map;
    printf("Serial_UpdateCurrentLevel: E%dM%d\n", current_episode, current_map);
}
/**
 * @brief Clear level mismatch flag (call after level load completes)
 */
void Serial_ClearLevelMismatch(void)
{
    level_mismatch_detected = false;
    mismatch_target_episode = 0;
    mismatch_target_map = 0;
    printf("Level mismatch flag cleared\n");
}
/**
 * @brief Check for incoming handshake packets (WAITING_FOR_START, READY_TO_START, GO)
 * Used during gameplay to detect if master has started a new level
 * @return ESP_OK if handshake packets detected, ESP_ERR_NOT_FOUND if none
 */
esp_err_t Serial_CheckHandshakePackets(void)
{
    // Check if we have any pending handshake packets
    if (pending_waiting_for_start || pending_ready_to_start || pending_go) {
        printf("NET: Found pending handshake packets: WAITING=%d, READY=%d, GO=%d\n",
               pending_waiting_for_start, pending_ready_to_start, pending_go);
        // DO NOT clear the flags here - they will be cleared in the handshake function
        // This allows the handshake to proceed correctly
        return ESP_OK;
    }
    // Try to receive packets to update pending flags
    uint8_t dummy[16];
    size_t dummy_len = 0;
    serial_net_receive(dummy, sizeof(dummy), &dummy_len);
    // Check again after receive attempt
    if (pending_waiting_for_start || pending_ready_to_start || pending_go) {
        printf("NET: Received handshake packets via poll\n");
        // Don't clear here - they were already cleared by the receive handler
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
bool serial_net_is_ready(void)
{
    return serial_initialized && handshake_complete;
}
uint32_t serial_net_get_free_heap(void)
{
    return esp_get_free_heap_size();
}
void serial_net_deinit(void)
{
    if (serial_initialized) {
        uart_driver_delete(UART_PORT);
        serial_initialized = false;
        handshake_complete = false;
        ESP_LOGI(SERIAL_TAG, "UART deinitialized");
    }
}
// ============================================================================
// Doom Network Interface Implementation
// Using Serial_ prefix to avoid collision with original Doom I_* functions
// ============================================================================
// Network statistics
static uint32_t stat_sent = 0;
static uint32_t stat_recv = 0;
static uint32_t stat_errors = 0;
// Ticcmd packet structure (transmitted over UART)
// Format: [PKT_TYPE (1)] [TIC (4 bytes little-endian)] [TICCMD (8 bytes)] = 13 bytes
#define TICCMD_PKT_SIZE 13
// Strict lock-step: Circular buffer for 5 ticcmds (allows for minor jitter/buffering)
// We block until we receive the exact tic we need, but buffer up to 5 future tics
#define TICCMD_QUEUE_SIZE 5
typedef struct {
    int tic;
    uint8_t cmd[8];  // ticcmd_t is 8 bytes
    bool valid;
} ticcmd_entry_t;
static ticcmd_entry_t ticcmd_queue[TICCMD_QUEUE_SIZE];
static int ticcmd_queue_head = 0;  // Oldest entry
static int ticcmd_queue_tail = 0;  // Next free slot
static int last_remote_tic = -1;   // Last tic received from remote
static int last_local_tic = -1;    // Last tic we sent
// Desync tracking
static int desync_count = 0;
#define DESYNC_WARN_THRESHOLD 2
esp_err_t Serial_NetSend(int tic, const void *ticcmd_data)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    // Build ticcmd packet: [TYPE][TIC:4][TICCMD:8]
    uint8_t pkt[TICCMD_PKT_SIZE];
    pkt[0] = NET_PKT_TICCMD;
    pkt[1] = (uint8_t)(tic & 0xFF);
    pkt[2] = (uint8_t)((tic >> 8) & 0xFF);
    pkt[3] = (uint8_t)((tic >> 16) & 0xFF);
    pkt[4] = (uint8_t)((tic >> 24) & 0xFF);
    memcpy(&pkt[5], ticcmd_data, 8);  // ticcmd_t is 8 bytes
    esp_err_t ret = serial_net_send(pkt, TICCMD_PKT_SIZE);
    if (ret == ESP_OK) {
        stat_sent++;
    }
    return ret;
}
esp_err_t Serial_GetPacket(int *out_tic, void *ticcmd_data)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    uint8_t pkt[TICCMD_PKT_SIZE];
    size_t len = 0;
    esp_err_t ret = serial_net_receive(pkt, TICCMD_PKT_SIZE, &len);
    if (ret == ESP_OK && len >= TICCMD_PKT_SIZE) {
        if (pkt[0] == NET_PKT_TICCMD) {
            // Extract tic number (little-endian)
            *out_tic = pkt[1] | (pkt[2] << 8) | (pkt[3] << 16) | (pkt[4] << 24);
            // Extract ticcmd
            memcpy(ticcmd_data, &pkt[5], 8);
            stat_recv++;
            return ESP_OK;
        } else {
            // Wrong packet type
            stat_errors++;
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
// Store received ticcmd in circular buffer (allows buffering of up to 5 tics)
static void queue_ticcmd(int tic, const uint8_t *cmd)
{
    // Find slot for this tic (or overwrite oldest if full)
    // Check if tic already exists in buffer
    for (int i = 0; i < TICCMD_QUEUE_SIZE; i++) {
        int idx = (ticcmd_queue_head + i) % TICCMD_QUEUE_SIZE;
        if (ticcmd_queue[idx].valid && ticcmd_queue[idx].tic == tic) {
            // Already have this tic, update it
            memcpy(ticcmd_queue[idx].cmd, cmd, 8);
            return;
        }
    }
    // Not found, add to next free slot
    int next_tail = (ticcmd_queue_tail + 1) % TICCMD_QUEUE_SIZE;
    if (next_tail == ticcmd_queue_head && ticcmd_queue[ticcmd_queue_tail].valid) {
        // Buffer full, overwrite oldest (head)
        ticcmd_queue_head = (ticcmd_queue_head + 1) % TICCMD_QUEUE_SIZE;
    }
    // Add to tail
    ticcmd_queue[ticcmd_queue_tail].tic = tic;
    memcpy(ticcmd_queue[ticcmd_queue_tail].cmd, cmd, 8);
    ticcmd_queue[ticcmd_queue_tail].valid = true;
    ticcmd_queue_tail = (ticcmd_queue_tail + 1) % TICCMD_QUEUE_SIZE;
    // Track for desync detection
    if (last_remote_tic >= 0 && tic != last_remote_tic + 1 && tic != last_remote_tic) {
        // Unexpected tic jump - log it
        desync_count++;
        if (!gameplay_log_suppressed) {
            printf("DESYNC: Expected tic %d or %d, got %d\n", 
                   last_remote_tic, last_remote_tic + 1, tic);
        }
    }
    last_remote_tic = tic;
}
// Get ticcmd for a specific tic from circular buffer (strict match)
static bool dequeue_ticcmd(int tic, uint8_t *cmd)
{
    for (int i = 0; i < TICCMD_QUEUE_SIZE; i++) {
        int idx = (ticcmd_queue_head + i) % TICCMD_QUEUE_SIZE;
        if (ticcmd_queue[idx].valid && ticcmd_queue[idx].tic == tic) {
            memcpy(cmd, ticcmd_queue[idx].cmd, 8);
            ticcmd_queue[idx].valid = false;
            // Advance head if this was the head
            if (idx == ticcmd_queue_head) {
                ticcmd_queue_head = (ticcmd_queue_head + 1) % TICCMD_QUEUE_SIZE;
            }
            return true;
        }
    }
    return false;
}
// Poll UART and queue any received ticcmds (non-blocking)
static void poll_and_queue_ticcmds(void)
{
    int recv_tic;
    uint8_t cmd[8];
    // Drain all available packets from UART into queue
    while (Serial_GetPacket(&recv_tic, cmd) == ESP_OK) {
        queue_ticcmd(recv_tic, cmd);
    }
}
// STRICT BLOCKING: Wait indefinitely (with safety timeout) for exact tic
// Game will stutter/freeze rather than desync
#define STRICT_SYNC_TIMEOUT_MS 2000  // 2 second safety timeout for normal tics
#define INITIAL_SYNC_TIMEOUT_MS 30000 // 30 second safety timeout for tic 0 (initial sync)
esp_err_t Serial_WaitForTiccmd(int expected_tic, void *ticcmd_data, int timeout_ms)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    // First check if we already have this tic queued
    if (dequeue_ticcmd(expected_tic, ticcmd_data)) {
        return ESP_OK;
    }
    int64_t start_time = esp_timer_get_time();
    int64_t timeout_us;
    // ALWAYS use extended timeout for tic 0 (initial synchronization)
    if (expected_tic == 0) {
        timeout_us = (int64_t)INITIAL_SYNC_TIMEOUT_MS * 1000;
    } else {
        timeout_us = (int64_t)STRICT_SYNC_TIMEOUT_MS * 1000;
    }
    // BLOCKING LOOP: Do NOT return until we get the expected tic
    while (1) {
        // Poll UART (non-blocking) and store received ticcmd
        poll_and_queue_ticcmds();
        // Check if we got the expected tic
        if (dequeue_ticcmd(expected_tic, ticcmd_data)) {
            return ESP_OK;
        }
        // EARLY EXIT: If level mismatch detected, return timeout immediately
        // so the game loop can load the correct level
        if (!is_master && level_mismatch_detected) {
            printf("SYNC: Level mismatch detected - exiting wait early to load correct level\n");
            return ESP_ERR_TIMEOUT;
        }
        // Safety timeout check (prevent infinite hang on disconnect)
        if ((esp_timer_get_time() - start_time) >= timeout_us) {
            printf("SYNC TIMEOUT: Waited %ldms for tic %d (last remote=%d)\n",
                   (long)(timeout_us / 1000), expected_tic, last_remote_tic);
            stat_errors++;
            desync_count++;
            return ESP_ERR_TIMEOUT;
        }
        // Yield to let other tasks run, but do NOT sleep for a full tick
        taskYIELD();
    }
}
void Serial_GetNetStats(uint32_t *sent_packets, uint32_t *recv_packets, uint32_t *checksum_errors)
{
    if (sent_packets) *sent_packets = stat_sent;
    if (recv_packets) *recv_packets = stat_recv;
    if (checksum_errors) *checksum_errors = stat_errors;
}
void serial_net_suppress_logging(bool suppress)
{
    gameplay_log_suppressed = suppress;
    if (suppress) {
        // Suppress all ESP_LOG output during gameplay to avoid UART1 interference
        // Console is on USB Serial/JTAG, but this ensures no stray output on UART1
        esp_log_level_set("*", ESP_LOG_NONE);
        ESP_LOGI(SERIAL_TAG, "Logging suppressed for gameplay");
    } else {
        // Restore normal logging
        esp_log_level_set("*", ESP_LOG_INFO);
        ESP_LOGI(SERIAL_TAG, "Logging restored");
    }
}
// Map change packet format: [PKT_TYPE (1)][EPISODE (1)][MAP (1)][SKILL (1)] = 4 bytes
#define MAP_CHANGE_PKT_SIZE 4
esp_err_t Serial_SendMapChange(int episode, int map, int skill)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    // Only master should send map change
    if (!is_master) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t pkt[MAP_CHANGE_PKT_SIZE];
    pkt[0] = NET_PKT_MAP_CHANGE;
    pkt[1] = (uint8_t)episode;
    pkt[2] = (uint8_t)map;
    pkt[3] = (uint8_t)skill;
    ESP_LOGI(SERIAL_TAG, "Sending MAP_CHANGE: E%dM%d skill=%d", episode, map, skill);
    return serial_net_send(pkt, MAP_CHANGE_PKT_SIZE);
}
esp_err_t Serial_CheckMapChange(int *out_episode, int *out_map, int *out_skill)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    uint8_t pkt[MAP_CHANGE_PKT_SIZE];
    size_t len = 0;
    esp_err_t ret = serial_net_receive(pkt, MAP_CHANGE_PKT_SIZE, &len);
    if (ret == ESP_OK && len >= MAP_CHANGE_PKT_SIZE && pkt[0] == NET_PKT_MAP_CHANGE) {
        *out_episode = pkt[1];
        *out_map = pkt[2];
        *out_skill = pkt[3];
        ESP_LOGI(SERIAL_TAG, "Received MAP_CHANGE: E%dM%d skill=%d", *out_episode, *out_map, *out_skill);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
void Serial_FlushBuffers(void)
{
    if (!serial_initialized) {
        return;
    }
    // Flush UART hardware buffers
    uart_flush(UART_PORT);
    // Clear ticcmd queue
    for (int i = 0; i < TICCMD_QUEUE_SIZE; i++) {
        ticcmd_queue[i].valid = false;
    }
    // Reset frame parser state
    frame_state = FRAME_WAIT_SYNC;
    frame_pos = 0;
    // Clear pending control packets to avoid stale level transitions
    pending_start_game = false;
    pending_start_skill = 0;
    pending_start_episode = 0;
    pending_start_map = 0;
    pending_ack_start = false;
    pending_waiting_for_start = false;
    pending_ready_to_start = false;
    pending_go = false;
    pending_extended_checksum = false;
    pending_extended_tic = 0;
    pending_extended_episode = 0;
    pending_extended_map = 0;
    pending_extended_p0_x = 0;
    pending_extended_p0_y = 0;
    pending_extended_p0_angle = 0;
    pending_extended_p0_health = 0;
    pending_extended_p1_x = 0;
    pending_extended_p1_y = 0;
    pending_extended_p1_angle = 0;
    pending_extended_p1_health = 0;
    // Reset tracking
    last_remote_tic = -1;
    last_local_tic = -1;
    desync_count = 0;
    ESP_LOGI(SERIAL_TAG, "Buffers flushed for new game");
}
int Serial_GetDesyncCount(void)
{
    return desync_count;
}
int Serial_GetLastRemoteTic(void)
{
    return last_remote_tic;
}
// Global desync flag - when true, game should halt (int for C compatibility)
int desync_detected = 0;
// ============================================================================
// Level-Start Blocking Handshake (PKT_LEVEL_READY)
// Both devices MUST complete this before first P_Ticker call
// ============================================================================
esp_err_t Serial_LevelReadySync(int timeout_ms)
{
    if (!serial_initialized || !handshake_complete) {
        printf("LEVEL SYNC: FAIL - serial not ready\n");
        return ESP_FAIL;
    }
    printf("LEVEL SYNC: Starting (timeout=%dms)...\n", timeout_ms);
    // Build level ready packet: [PKT_TYPE]
    uint8_t pkt[1] = { NET_PKT_LEVEL_READY };
    bool sent = false;
    bool received = false;
    int64_t start_time = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    int send_interval_us = 100000;  // Send every 100ms
    int64_t last_send_time = 0;
    int loop_count = 0;
    while (!(received && sent)) {  // Exit when BOTH are true
        int64_t now = esp_timer_get_time();
        loop_count++;
        // Timeout check - HARD EXIT
        if ((now - start_time) >= timeout_us) {
            printf("LEVEL SYNC: TIMEOUT after %d loops (sent=%d recv=%d)\n", 
                   loop_count, sent, received);
            return ESP_ERR_TIMEOUT;
        }
        // Debug every 50 loops (~1 second)
        if (loop_count % 50 == 0) {
            printf("LEVEL SYNC: Waiting... loop=%d sent=%d recv=%d\n", 
                   loop_count, sent, received);
        }
        // Send our ready packet periodically
        if ((now - last_send_time) >= send_interval_us) {
            serial_net_send(pkt, 1);
            sent = true;
            last_send_time = now;
        }
        // Check for incoming level ready packet
        uint8_t rx_pkt[4];
        size_t rx_len = 0;
        if (serial_net_receive(rx_pkt, sizeof(rx_pkt), &rx_len) == ESP_OK) {
            if (rx_len >= 1 && rx_pkt[0] == NET_PKT_LEVEL_READY) {
                received = true;
                printf("LEVEL SYNC: Got PKT_LEVEL_READY!\n");
            }
        }
        // Small delay to prevent tight spin
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Both sent and received - we're synced!
    printf("LEVEL SYNC: SUCCESS after %d loops\n", loop_count);
    // CRITICAL: Clear any stale packets that arrived during level loading
    // These would otherwise trigger spurious level reload or double transitions
    pending_waiting_for_start = false;
    pending_ready_to_start = false;
    pending_go = false;
    printf("LEVEL SYNC: Cleared stale handshake flags\n");
    return ESP_OK;
}
// ============================================================================
// Startup Sync (NET_PKT_STARTUP_READY)
// Ensures both devices are ready before entering doom_main
// ============================================================================
esp_err_t Serial_StartupSync(int timeout_ms)
{
    if (!serial_initialized || !handshake_complete) {
        printf("STARTUP SYNC: FAIL - serial not ready\n");
        return ESP_FAIL;
    }
    printf("STARTUP SYNC: Starting (timeout=%dms) as %s\n",
           timeout_ms, is_master ? "MASTER" : "SLAVE");
    uint8_t pkt[1] = { NET_PKT_STARTUP_READY };
    bool sent = false;
    bool received = false;
    int64_t start_time = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    int64_t last_send_time = 0;
    const int send_interval_us = 100000; // 100ms
    while (!(sent && received)) {
        int64_t now = esp_timer_get_time();
        if ((now - start_time) >= timeout_us) {
            printf("STARTUP SYNC: TIMEOUT (sent=%d recv=%d)\n", sent, received);
            return ESP_ERR_TIMEOUT;
        }
        if ((now - last_send_time) >= send_interval_us) {
            serial_net_send(pkt, 1);
            sent = true;
            last_send_time = now;
        }
        uint8_t rx_pkt[8];
        size_t rx_len = 0;
        if (serial_net_receive(rx_pkt, sizeof(rx_pkt), &rx_len) == ESP_OK) {
            if (rx_len >= 1 && rx_pkt[0] == NET_PKT_STARTUP_READY) {
                received = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("STARTUP SYNC: SUCCESS\n");
    return ESP_OK;
}
// ============================================================================
// Position Checksum for Desync Detection (PKT_CHKSUM)
// Master sends every 64 tics, Slave compares and halts on mismatch
// ============================================================================
// Checksum packet format: [PKT_TYPE(1)][TIC(4)][X(4)][Y(4)] = 13 bytes
#define CHKSUM_PKT_SIZE 13
esp_err_t Serial_SendChecksum(int tic, int player0_x, int player0_y)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    uint8_t pkt[CHKSUM_PKT_SIZE];
    pkt[0] = NET_PKT_CHKSUM;
    // Tic (little-endian)
    pkt[1] = (uint8_t)(tic & 0xFF);
    pkt[2] = (uint8_t)((tic >> 8) & 0xFF);
    pkt[3] = (uint8_t)((tic >> 16) & 0xFF);
    pkt[4] = (uint8_t)((tic >> 24) & 0xFF);
    // X position (little-endian)
    pkt[5] = (uint8_t)(player0_x & 0xFF);
    pkt[6] = (uint8_t)((player0_x >> 8) & 0xFF);
    pkt[7] = (uint8_t)((player0_x >> 16) & 0xFF);
    pkt[8] = (uint8_t)((player0_x >> 24) & 0xFF);
    // Y position (little-endian)
    pkt[9] = (uint8_t)(player0_y & 0xFF);
    pkt[10] = (uint8_t)((player0_y >> 8) & 0xFF);
    pkt[11] = (uint8_t)((player0_y >> 16) & 0xFF);
    pkt[12] = (uint8_t)((player0_y >> 24) & 0xFF);
    return serial_net_send(pkt, CHKSUM_PKT_SIZE);
}
esp_err_t Serial_CheckChecksum(int *out_tic, int *out_x, int *out_y)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    uint8_t pkt[CHKSUM_PKT_SIZE];
    size_t len = 0;
    esp_err_t ret = serial_net_receive(pkt, CHKSUM_PKT_SIZE, &len);
    if (ret == ESP_OK && len >= CHKSUM_PKT_SIZE && pkt[0] == NET_PKT_CHKSUM) {
        // Extract tic (little-endian)
        *out_tic = pkt[1] | (pkt[2] << 8) | (pkt[3] << 16) | (pkt[4] << 24);
        // Extract X (little-endian)
        *out_x = pkt[5] | (pkt[6] << 8) | (pkt[7] << 16) | (pkt[8] << 24);
        // Extract Y (little-endian)
        *out_y = pkt[9] | (pkt[10] << 8) | (pkt[11] << 16) | (pkt[12] << 24);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
// ============================================================================
// Start Game Handshake (PKT_START_GAME / PKT_ACK_START)
// Two-stage handshake: Master sends START_GAME, waits for ACK_START from Slave
// ============================================================================
// Start game packet format: [PKT_TYPE(1)][SKILL(1)][EPISODE(1)][MAP(1)] = 4 bytes
#define START_GAME_PKT_SIZE 4
// Internal: Send raw START_GAME packet once
static esp_err_t send_start_game_packet(int skill, int episode, int map)
{
    uint8_t pkt[START_GAME_PKT_SIZE];
    pkt[0] = NET_PKT_START_GAME;
    pkt[1] = (uint8_t)skill;
    pkt[2] = (uint8_t)episode;
    pkt[3] = (uint8_t)map;
    return serial_net_send(pkt, START_GAME_PKT_SIZE);
}
// Master: Send START_GAME repeatedly until ACK_START received
esp_err_t Serial_StartGameHandshake(int skill, int episode, int map, int timeout_ms)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    printf("MASTER: Starting game handshake - skill=%d ep=%d map=%d\n", skill, episode, map);
    pending_ack_start = false;
    int64_t start_time = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    int send_count = 0;
    while ((esp_timer_get_time() - start_time) < timeout_us) {
        // Send START_GAME packet every 100ms
        if (send_count % 5 == 0) {  // Every 5th iteration (100ms)
            send_start_game_packet(skill, episode, map);
            printf("MASTER: Sent START_GAME #%d, waiting for Slave ACK...\n", send_count / 5 + 1);
        }
        send_count++;
        // Check for ACK_START
        uint8_t pkt[16];
        size_t len = 0;
        if (serial_net_receive(pkt, sizeof(pkt), &len) == ESP_OK) {
            if (len >= 1 && pkt[0] == NET_PKT_ACK_START) {
                printf("MASTER: Received ACK_START from Slave - handshake complete!\n");
                return ESP_OK;
            }
        }
        // Also check pending flag (in case receive captured it elsewhere)
        if (pending_ack_start) {
            pending_ack_start = false;
            printf("MASTER: ACK_START detected (pending) - handshake complete!\n");
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("MASTER: START_GAME handshake TIMEOUT - Slave did not respond!\n");
    return ESP_ERR_TIMEOUT;
}
// Slave: Send ACK_START to master
esp_err_t Serial_SendStartAck(void)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    printf("SLAVE: Sending ACK_START to Master\n");
    uint8_t pkt[1] = { NET_PKT_ACK_START };
    // Send multiple times for reliability
    for (int i = 0; i < 3; i++) {
        serial_net_send(pkt, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}
// Slave: Check for START_GAME packet
esp_err_t Serial_CheckStartGame(int *out_skill, int *out_episode, int *out_map)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    // First check if we have a pending START_GAME from earlier receives
    if (pending_start_game) {
        *out_skill = pending_start_skill;
        *out_episode = pending_start_episode;
        *out_map = pending_start_map;
        pending_start_game = false;  // Clear the pending flag
        printf("SLAVE: Found pending START_GAME - skill=%d ep=%d map=%d\n", 
               *out_skill, *out_episode, *out_map);
        return ESP_OK;
    }
    // Try to receive more packets (this will capture START_GAME into pending buffer)
    uint8_t pkt[16];
    size_t len = 0;
    serial_net_receive(pkt, sizeof(pkt), &len);
    // Check again after receive attempt
    if (pending_start_game) {
        *out_skill = pending_start_skill;
        *out_episode = pending_start_episode;
        *out_map = pending_start_map;
        pending_start_game = false;
        printf("SLAVE: Received START_GAME - skill=%d ep=%d map=%d\n", 
               *out_skill, *out_episode, *out_map);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
// ============================================================================
// 3-Way Handshake for Level Loading Synchronization
// Master: Send PKT_WAITING_FOR_START, wait for PKT_READY_TO_START, send PKT_GO
// Slave: Wait for PKT_WAITING_FOR_START, send PKT_READY_TO_START, wait for PKT_GO
// ============================================================================
esp_err_t Serial_LevelLoad3WayHandshake(int timeout_ms)
{
    if (!serial_initialized || !handshake_complete) {
        printf("3WAY: FAIL - serial not ready\n");
        return ESP_FAIL;
    }
    printf("3WAY: Starting 3-way handshake (timeout=%dms) as %s\n", 
           timeout_ms, is_master ? "MASTER" : "SLAVE");
    int64_t start_time = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    if (is_master) {
        // =========== MASTER PROTOCOL ===========
        // Step 1: Send PKT_WAITING_FOR_START
        printf("3WAY MASTER: Step 1 - Sending PKT_WAITING_FOR_START\n");
        uint8_t waiting_pkt[1] = { NET_PKT_WAITING_FOR_START };
        for (int i = 0; i < 5; i++) {
            serial_net_send(waiting_pkt, 1);
            uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(10));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        pending_waiting_for_start = false; // We sent it, so clear any pending
        // Step 2: Wait for slave's PKT_READY_TO_START
        printf("3WAY MASTER: Step 2 - Waiting for PKT_READY_TO_START from slave\n");
        while ((esp_timer_get_time() - start_time) < timeout_us) {
            // Check pending flag (set by receive handler)
            if (pending_ready_to_start) {
                pending_ready_to_start = false;
                printf("3WAY MASTER: Received PKT_READY_TO_START from slave\n");
                break;
            }
            // Also poll receive in case pending flag was missed
            uint8_t pkt[16];
            size_t len = 0;
            if (serial_net_receive(pkt, sizeof(pkt), &len) == ESP_OK) {
                if (len >= 1 && pkt[0] == NET_PKT_READY_TO_START) {
                    printf("3WAY MASTER: Received PKT_READY_TO_START via poll\n");
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        // Check timeout
        if ((esp_timer_get_time() - start_time) >= timeout_us) {
            printf("3WAY MASTER: TIMEOUT waiting for slave READY\n");
            return ESP_ERR_TIMEOUT;
        }
        // Step 3: Send PKT_GO to slave
        printf("3WAY MASTER: Step 3 - Sending PKT_GO to slave\n");
        uint8_t go_pkt[1] = { NET_PKT_GO };
        for (int i = 0; i < 5; i++) {
            serial_net_send(go_pkt, 1);
            uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(10));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        pending_go = false; // We sent it, so clear any pending
        printf("3WAY MASTER: Handshake complete - both devices can start level\n");
        return ESP_OK;
    } else {
        // =========== SLAVE PROTOCOL ===========
        // Step 1: Wait for master's PKT_WAITING_FOR_START
        printf("3WAY SLAVE: Step 1 - Waiting for PKT_WAITING_FOR_START from master\n");
        // Check if we already have a GO packet (received during gameplay)
        if (pending_go) {
            pending_go = false;
            printf("3WAY SLAVE: Already have GO packet from master, proceeding to level load\n");
            return ESP_OK;
        }
        // Check if we already have WAITING_FOR_START - skip waiting if so
        bool have_waiting = pending_waiting_for_start;
        if (have_waiting) {
            pending_waiting_for_start = false;
            printf("3WAY SLAVE: Already have WAITING_FOR_START from master, proceeding to step 2\n");
        }
        if (!have_waiting) {
            // Give master time to send WAITING packet
            vTaskDelay(pdMS_TO_TICKS(200));
            while ((esp_timer_get_time() - start_time) < timeout_us) {
                if (pending_go) {
                    pending_go = false;
                    printf("3WAY SLAVE: Already have GO packet from master, proceeding to level load\n");
                    return ESP_OK;
                }
                if (pending_waiting_for_start) {
                    pending_waiting_for_start = false;
                    printf("3WAY SLAVE: Received PKT_WAITING_FOR_START from master\n");
                    break;
                }
                // Poll receive
                uint8_t pkt[16];
                size_t len = 0;
                if (serial_net_receive(pkt, sizeof(pkt), &len) == ESP_OK) {
                    if (len >= 1 && pkt[0] == NET_PKT_WAITING_FOR_START) {
                        printf("3WAY SLAVE: Received PKT_WAITING_FOR_START via poll\n");
                        break;
                    }
                    if (len >= 1 && pkt[0] == NET_PKT_GO) {
                        pending_go = true;
                        printf("3WAY SLAVE: Received GO packet while waiting for WAITING\n");
                        // Continue to check pending_go in next iteration
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if ((esp_timer_get_time() - start_time) >= timeout_us) {
                printf("3WAY SLAVE: TIMEOUT waiting for master WAITING\n");
                return ESP_ERR_TIMEOUT;
            }
        }
        // Step 2: Send PKT_READY_TO_START to master
        printf("3WAY SLAVE: Step 2 - Sending PKT_READY_TO_START to master\n");
        uint8_t ready_pkt[1] = { NET_PKT_READY_TO_START };
        for (int i = 0; i < 5; i++) {
            serial_net_send(ready_pkt, 1);
            uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(10));
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        pending_ready_to_start = false; // We sent it, so clear any pending
        // Step 3: Wait for master's PKT_GO
        printf("3WAY SLAVE: Step 3 - Waiting for PKT_GO from master\n");
        while ((esp_timer_get_time() - start_time) < timeout_us) {
            if (pending_go) {
                pending_go = false;
                printf("3WAY SLAVE: Received PKT_GO from master\n");
                break;
            }
            // Poll receive
            uint8_t pkt[16];
            size_t len = 0;
            if (serial_net_receive(pkt, sizeof(pkt), &len) == ESP_OK) {
                if (len >= 1 && pkt[0] == NET_PKT_GO) {
                    printf("3WAY SLAVE: Received PKT_GO via poll\n");
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if ((esp_timer_get_time() - start_time) >= timeout_us) {
            printf("3WAY SLAVE: TIMEOUT waiting for master GO\n");
            return ESP_ERR_TIMEOUT;
        }
        printf("3WAY SLAVE: Handshake complete - ready to start level\n");
        return ESP_OK;
    }
}
// ============================================================================
// Extended Checksum for Zero-Tolerance Player State Sync (PKT_EXTENDED_CHKSUM)
// Master sends every 16 tics, Slave compares positions/angles/health
// NEW: Also includes episode/map for level sync detection
// Packet format: [PKT_TYPE(1)][TIC(4)][EPISODE(1)][MAP(1)][P0_X(4)][P0_Y(4)][P0_ANGLE(4)][P0_HEALTH(4)][P1_X(4)][P1_Y(4)][P1_ANGLE(4)][P1_HEALTH(4)] = 39 bytes
// ============================================================================
#define EXTENDED_CHKSUM_PKT_SIZE 39
esp_err_t Serial_SendExtendedChecksum(int tic, int episode, int map,
    int p0_x, int p0_y, int p0_angle, int p0_health,
    int p1_x, int p1_y, int p1_angle, int p1_health)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    uint8_t pkt[EXTENDED_CHKSUM_PKT_SIZE];
    int idx = 0;
    pkt[idx++] = NET_PKT_EXTENDED_CHKSUM;
    // Tic (little-endian)
    pkt[idx++] = (uint8_t)(tic & 0xFF);
    pkt[idx++] = (uint8_t)((tic >> 8) & 0xFF);
    pkt[idx++] = (uint8_t)((tic >> 16) & 0xFF);
    pkt[idx++] = (uint8_t)((tic >> 24) & 0xFF);
    // Episode and Map (1-based)
    pkt[idx++] = (uint8_t)episode;
    pkt[idx++] = (uint8_t)map;
    // Player 0 X (little-endian)
    pkt[idx++] = (uint8_t)(p0_x & 0xFF);
    pkt[idx++] = (uint8_t)((p0_x >> 8) & 0xFF);
    pkt[idx++] = (uint8_t)((p0_x >> 16) & 0xFF);
    pkt[idx++] = (uint8_t)((p0_x >> 24) & 0xFF);
    // Player 0 Y (little-endian)
    pkt[idx++] = (uint8_t)(p0_y & 0xFF);
    pkt[idx++] = (uint8_t)((p0_y >> 8) & 0xFF);
    pkt[idx++] = (uint8_t)((p0_y >> 16) & 0xFF);
    pkt[idx++] = (uint8_t)((p0_y >> 24) & 0xFF);
    // Player 0 angle (little-endian)
    pkt[idx++] = (uint8_t)(p0_angle & 0xFF);
    pkt[idx++] = (uint8_t)((p0_angle >> 8) & 0xFF);
    pkt[idx++] = (uint8_t)((p0_angle >> 16) & 0xFF);
    pkt[idx++] = (uint8_t)((p0_angle >> 24) & 0xFF);
    // Player 0 health (little-endian)
    pkt[idx++] = (uint8_t)(p0_health & 0xFF);
    pkt[idx++] = (uint8_t)((p0_health >> 8) & 0xFF);
    pkt[idx++] = (uint8_t)((p0_health >> 16) & 0xFF);
    pkt[idx++] = (uint8_t)((p0_health >> 24) & 0xFF);
    // Player 1 X (little-endian)
    pkt[idx++] = (uint8_t)(p1_x & 0xFF);
    pkt[idx++] = (uint8_t)((p1_x >> 8) & 0xFF);
    pkt[idx++] = (uint8_t)((p1_x >> 16) & 0xFF);
    pkt[idx++] = (uint8_t)((p1_x >> 24) & 0xFF);
    // Player 1 Y (little-endian)
    pkt[idx++] = (uint8_t)(p1_y & 0xFF);
    pkt[idx++] = (uint8_t)((p1_y >> 8) & 0xFF);
    pkt[idx++] = (uint8_t)((p1_y >> 16) & 0xFF);
    pkt[idx++] = (uint8_t)((p1_y >> 24) & 0xFF);
    // Player 1 angle (little-endian)
    pkt[idx++] = (uint8_t)(p1_angle & 0xFF);
    pkt[idx++] = (uint8_t)((p1_angle >> 8) & 0xFF);
    pkt[idx++] = (uint8_t)((p1_angle >> 16) & 0xFF);
    pkt[idx++] = (uint8_t)((p1_angle >> 24) & 0xFF);
    // Player 1 health (little-endian)
    pkt[idx++] = (uint8_t)(p1_health & 0xFF);
    pkt[idx++] = (uint8_t)((p1_health >> 8) & 0xFF);
    pkt[idx++] = (uint8_t)((p1_health >> 16) & 0xFF);
    pkt[idx++] = (uint8_t)((p1_health >> 24) & 0xFF);
    ESP_LOGI(SERIAL_TAG, "Sending EXTENDED_CHKSUM tic=%d E%dM%d P0(%d,%d,%d,%d) P1(%d,%d,%d,%d)",
             tic, episode, map, p0_x, p0_y, p0_angle, p0_health, p1_x, p1_y, p1_angle, p1_health);
    return serial_net_send(pkt, EXTENDED_CHKSUM_PKT_SIZE);
}
esp_err_t Serial_CheckExtendedChecksum(int *out_tic, int *out_episode, int *out_map,
    int *out_p0_x, int *out_p0_y, int *out_p0_angle, int *out_p0_health,
    int *out_p1_x, int *out_p1_y, int *out_p1_angle, int *out_p1_health)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    // Check if we have a pending extended checksum (set by receive handler)
    if (pending_extended_checksum) {
        *out_tic = pending_extended_tic;
        *out_episode = pending_extended_episode;
        *out_map = pending_extended_map;
        *out_p0_x = pending_extended_p0_x;
        *out_p0_y = pending_extended_p0_y;
        *out_p0_angle = pending_extended_p0_angle;
        *out_p0_health = pending_extended_p0_health;
        *out_p1_x = pending_extended_p1_x;
        *out_p1_y = pending_extended_p1_y;
        *out_p1_angle = pending_extended_p1_angle;
        *out_p1_health = pending_extended_p1_health;
        pending_extended_checksum = false;  // Clear the pending flag
        ESP_LOGI(SERIAL_TAG, "Found pending EXTENDED_CHKSUM tic=%d E%dM%d", *out_tic, *out_episode, *out_map);
        return ESP_OK;
    }
    // Try to receive more packets (this will capture EXTENDED_CHKSUM into pending buffer)
    uint8_t pkt[EXTENDED_CHKSUM_PKT_SIZE];
    size_t len = 0;
    serial_net_receive(pkt, sizeof(pkt), &len);
    // Check again after receive attempt
    if (pending_extended_checksum) {
        *out_tic = pending_extended_tic;
        *out_episode = pending_extended_episode;
        *out_map = pending_extended_map;
        *out_p0_x = pending_extended_p0_x;
        *out_p0_y = pending_extended_p0_y;
        *out_p0_angle = pending_extended_p0_angle;
        *out_p0_health = pending_extended_p0_health;
        *out_p1_x = pending_extended_p1_x;
        *out_p1_y = pending_extended_p1_y;
        *out_p1_angle = pending_extended_p1_angle;
        *out_p1_health = pending_extended_p1_health;
        pending_extended_checksum = false;
        ESP_LOGI(SERIAL_TAG, "Received EXTENDED_CHKSUM tic=%d E%dM%d", *out_tic, *out_episode, *out_map);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
// Check if master is on a different level than slave
// This is called from game code to detect level mismatch and trigger level load
// Pass current episode/map, function returns target episode/map if mismatch
esp_err_t Serial_CheckLevelMismatch(int current_episode, int current_map, 
                                     int *out_target_episode, int *out_target_map)
{
    if (!serial_initialized || !handshake_complete) {
        return ESP_FAIL;
    }
    // Only slave should check for mismatch
    if (is_master) {
        return ESP_ERR_NOT_FOUND;
    }
    // Poll for any new packets (this updates mismatch_target_* variables)
    uint8_t pkt[64];
    size_t len = 0;
    serial_net_receive(pkt, sizeof(pkt), &len);
    // Check if we have a valid target level that differs from current
    // Only trigger mismatch if slave already has a loaded level (episode/map > 0)
    // This prevents false triggers during initial attachment when slave's level is not yet set
    if (mismatch_target_episode > 0 && mismatch_target_map > 0 && current_episode > 0 && current_map > 0) {
        if (mismatch_target_episode != current_episode || mismatch_target_map != current_map) {
            printf("LEVEL MISMATCH CHECK: Master is on E%dM%d, Slave is on E%dM%d\n",
                   mismatch_target_episode, mismatch_target_map, current_episode, current_map);
            *out_target_episode = mismatch_target_episode;
            *out_target_map = mismatch_target_map;
            level_mismatch_detected = true;
            return ESP_OK;  // Mismatch detected!
        }
    }
    // No mismatch - clear the flag
    level_mismatch_detected = false;
    return ESP_ERR_NOT_FOUND;
}
// Slave: Send current level to master (call when level changes)
esp_err_t Serial_SendSlaveLevel(int episode, int map)
{
    if (!serial_initialized || !handshake_complete || is_master) {
        return ESP_FAIL;  // Only slave can send this
    }
    uint8_t pkt[3];
    pkt[0] = NET_PKT_SLAVE_LEVEL;
    pkt[1] = (uint8_t)episode;
    pkt[2] = (uint8_t)map;
    printf("NET TX: Sending SLAVE_LEVEL (E%dM%d)\n", episode, map);
    // Send multiple times for reliability (3 times)
    esp_err_t last_result = ESP_OK;
    for (int i = 0; i < 3; i++) {
        last_result = serial_net_send(pkt, 3);
        if (last_result != ESP_OK) {
            printf("NET TX: SLAVE_LEVEL attempt %d failed: %d\n", i, last_result);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return last_result;
}
// Master: Check for slave's current level (to detect slave level changes)
esp_err_t Serial_CheckSlaveLevel(int *out_episode, int *out_map)
{
    if (!serial_initialized || !handshake_complete || !is_master) {
        return ESP_FAIL;  // Only master can check this
    }
    // Check if we have a pending slave level packet
    if (pending_slave_level) {
        *out_episode = pending_slave_episode;
        *out_map = pending_slave_map;
        pending_slave_level = false;  // Clear the pending flag
        printf("NET: Found pending SLAVE_LEVEL (E%dM%d)\n", *out_episode, *out_map);
        return ESP_OK;
    }
    // Try to receive more packets (this will capture SLAVE_LEVEL into pending buffer)
    uint8_t pkt[16];
    size_t len = 0;
    serial_net_receive(pkt, sizeof(pkt), &len);
    // Check again after receive attempt
    if (pending_slave_level) {
        *out_episode = pending_slave_episode;
        *out_map = pending_slave_map;
        pending_slave_level = false;
        printf("NET: Received SLAVE_LEVEL via poll (E%dM%d)\n", *out_episode, *out_map);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}