/**
 * @file i_net_espnow.c
 * @brief ESP-NOW multiplayer networking implementation
 */
#include "i_net_espnow.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

#define ESPNOW_TAG "ESPNOW"
#define ESPNOW_CHANNEL 1
#define ESPNOW_QUEUE_SIZE 6
#define SYNC_MAGIC "DOOM_SYNC"

// Use extern - these are defined in i_net_serial.c (only one definition allowed)
extern bool is_multiplayer;
extern bool is_master;

static uint8_t peer_mac[ESP_NOW_ETH_ALEN] = {0};
static bool peer_paired = false;
static QueueHandle_t espnow_queue = NULL;
static SemaphoreHandle_t send_mutex = NULL;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t data[250];
    int data_len;
} espnow_event_t;

// ESP-NOW receive callback
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (len <= 0) return;
    
    espnow_event_t evt;
    memcpy(evt.mac_addr, recv_info->src_addr, ESP_NOW_ETH_ALEN);
    evt.data_len = len;
    memcpy(evt.data, data, len);
    
    xQueueSend(espnow_queue, &evt, 0);
}

// ESP-NOW send callback
static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    // Optional: Track send success/failure
    (void)mac_addr;
    (void)status;
}

esp_err_t espnow_init(bool master)
{
    ESP_LOGI(ESPNOW_TAG, "Initializing ESP-NOW (mode: %s)", master ? "Master" : "Slave");
    
    is_master = master;
    is_multiplayer = true;
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize WiFi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    
    // Log free heap after WiFi init
    uint32_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(ESPNOW_TAG, "Free heap after WiFi init: %lu bytes", free_heap);
    
    // Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    
    // Set PMK (optional encryption key)
    // ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)"pmk1234567890123"));
    
    // Create queue and mutex
    espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    send_mutex = xSemaphoreCreateMutex();
    
    if (!espnow_queue || !send_mutex) {
        ESP_LOGE(ESPNOW_TAG, "Failed to create queue/mutex");
        return ESP_FAIL;
    }
    
    ESP_LOGI(ESPNOW_TAG, "ESP-NOW initialized successfully");
    return ESP_OK;
}

esp_err_t espnow_pair(void)
{
    ESP_LOGI(ESPNOW_TAG, "Starting pairing process...");
    
    uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t sync_msg[16];
    strcpy((char*)sync_msg, SYNC_MAGIC);
    
    if (is_master) {
        // Master: Broadcast sync message
        ESP_LOGI(ESPNOW_TAG, "Master: Broadcasting DOOM_SYNC...");
        
        esp_now_peer_info_t peer_info = {0};
        memcpy(peer_info.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
        peer_info.channel = ESPNOW_CHANNEL;
        peer_info.ifidx = ESP_IF_WIFI_STA;
        peer_info.encrypt = false;
        
        esp_now_add_peer(&peer_info);
        
        // Send sync packets for 5 seconds
        for (int i = 0; i < 10; i++) {
            esp_now_send(broadcast_mac, sync_msg, strlen(SYNC_MAGIC) + 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            
            // Check for slave response
            espnow_event_t evt;
            if (xQueueReceive(espnow_queue, &evt, 0) == pdTRUE) {
                if (strncmp((char*)evt.data, SYNC_MAGIC, strlen(SYNC_MAGIC)) == 0) {
                    memcpy(peer_mac, evt.mac_addr, ESP_NOW_ETH_ALEN);
                    ESP_LOGI(ESPNOW_TAG, "Master: Slave paired! MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                             peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
                    
                    // Add slave as peer
                    esp_now_del_peer(broadcast_mac);
                    esp_now_peer_info_t slave_peer = {0};
                    memcpy(slave_peer.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
                    slave_peer.channel = ESPNOW_CHANNEL;
                    slave_peer.ifidx = ESP_IF_WIFI_STA;
                    slave_peer.encrypt = false;
                    esp_now_add_peer(&slave_peer);
                    
                    peer_paired = true;
                    return ESP_OK;
                }
            }
        }
        
        ESP_LOGE(ESPNOW_TAG, "Master: Pairing timeout");
        return ESP_FAIL;
        
    } else {
        // Slave: Listen for master sync and reply
        ESP_LOGI(ESPNOW_TAG, "Slave: Listening for DOOM_SYNC...");
        
        espnow_event_t evt;
        for (int i = 0; i < 10; i++) {
            if (xQueueReceive(espnow_queue, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {
                if (strncmp((char*)evt.data, SYNC_MAGIC, strlen(SYNC_MAGIC)) == 0) {
                    memcpy(peer_mac, evt.mac_addr, ESP_NOW_ETH_ALEN);
                    ESP_LOGI(ESPNOW_TAG, "Slave: Master found! MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                             peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
                    
                    // Add master as peer
                    esp_now_peer_info_t master_peer = {0};
                    memcpy(master_peer.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
                    master_peer.channel = ESPNOW_CHANNEL;
                    master_peer.ifidx = ESP_IF_WIFI_STA;
                    master_peer.encrypt = false;
                    esp_now_add_peer(&master_peer);
                    
                    // Reply to master
                    esp_now_send(peer_mac, sync_msg, strlen(SYNC_MAGIC) + 1);
                    
                    peer_paired = true;
                    return ESP_OK;
                }
            }
        }
        
        ESP_LOGE(ESPNOW_TAG, "Slave: Pairing timeout");
        return ESP_FAIL;
    }
}

esp_err_t espnow_send(const uint8_t *data, size_t len)
{
    if (!peer_paired || len > 250) {
        return ESP_FAIL;
    }
    
    xSemaphoreTake(send_mutex, portMAX_DELAY);
    esp_err_t ret = esp_now_send(peer_mac, data, len);
    xSemaphoreGive(send_mutex);
    
    return ret;
}

esp_err_t espnow_receive(uint8_t *data, size_t *len)
{
    espnow_event_t evt;
    
    if (xQueueReceive(espnow_queue, &evt, 0) == pdTRUE) {
        // Skip sync messages
        if (strncmp((char*)evt.data, SYNC_MAGIC, strlen(SYNC_MAGIC)) == 0) {
            return ESP_ERR_NOT_FOUND;
        }
        
        memcpy(data, evt.data, evt.data_len);
        *len = evt.data_len;
        return ESP_OK;
    }
    
    return ESP_ERR_NOT_FOUND;
}

bool espnow_is_ready(void)
{
    return peer_paired;
}

uint32_t espnow_get_free_heap(void)
{
    return esp_get_free_heap_size();
}

static bool callbacks_suspended = false;

void espnow_suspend_callbacks(void)
{
    if (callbacks_suspended) return;
    
    ESP_LOGI(ESPNOW_TAG, "Suspending ESP-NOW callbacks for safe Doom init...");
    
    // Unregister callbacks to prevent interference during heavy initialization
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    
    // Flush any pending queue items
    espnow_flush_queue();
    
    callbacks_suspended = true;
    ESP_LOGI(ESPNOW_TAG, "ESP-NOW callbacks suspended");
}

void espnow_resume_callbacks(void)
{
    if (!callbacks_suspended) return;
    
    ESP_LOGI(ESPNOW_TAG, "Resuming ESP-NOW callbacks...");
    
    // Re-register callbacks
    esp_now_register_send_cb(espnow_send_cb);
    esp_now_register_recv_cb(espnow_recv_cb);
    
    callbacks_suspended = false;
    ESP_LOGI(ESPNOW_TAG, "ESP-NOW callbacks resumed - ready for multiplayer");
}

void espnow_flush_queue(void)
{
    if (!espnow_queue) return;
    
    espnow_event_t evt;
    int flushed = 0;
    while (xQueueReceive(espnow_queue, &evt, 0) == pdTRUE) {
        flushed++;
    }
    
    if (flushed > 0) {
        ESP_LOGI(ESPNOW_TAG, "Flushed %d pending ESP-NOW messages", flushed);
    }
}

static bool wifi_deinitialized = false;

void espnow_deinit_wifi(void)
{
    if (wifi_deinitialized) {
        ESP_LOGW(ESPNOW_TAG, "WiFi already deinitialized");
        return;
    }
    
    ESP_LOGI(ESPNOW_TAG, "=== DEINITIALIZING WIFI TO FREE MEMORY ===");
    uint32_t heap_before = esp_get_free_heap_size();
    ESP_LOGI(ESPNOW_TAG, "Free heap BEFORE deinit: %lu bytes", heap_before);
    
    // peer_mac is already saved from pairing, no need to save again
    ESP_LOGI(ESPNOW_TAG, "Peer MAC saved: %02X:%02X:%02X:%02X:%02X:%02X",
             peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
    
    // Unregister callbacks first
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    
    // Delete peer
    esp_now_del_peer(peer_mac);
    
    // Deinit ESP-NOW
    esp_now_deinit();
    
    // Stop and deinit WiFi
    esp_wifi_stop();
    esp_wifi_deinit();
    
    // Delete queue and mutex to free more memory
    if (espnow_queue) {
        vQueueDelete(espnow_queue);
        espnow_queue = NULL;
    }
    if (send_mutex) {
        vSemaphoreDelete(send_mutex);
        send_mutex = NULL;
    }
    
    wifi_deinitialized = true;
    callbacks_suspended = false;
    
    // Force a garbage collection / memory consolidation
    vTaskDelay(pdMS_TO_TICKS(50));
    
    uint32_t heap_after = esp_get_free_heap_size();
    ESP_LOGI(ESPNOW_TAG, "Free heap AFTER deinit: %lu bytes", heap_after);
    ESP_LOGI(ESPNOW_TAG, "=== FREED %lu BYTES BY DEINIT WIFI ===", heap_after - heap_before);
}

esp_err_t espnow_reinit_wifi(void)
{
    if (!wifi_deinitialized) {
        ESP_LOGW(ESPNOW_TAG, "WiFi not deinitialized, nothing to reinit");
        return ESP_OK;
    }
    
    if (!peer_paired) {
        ESP_LOGE(ESPNOW_TAG, "Cannot reinit - no peer was paired");
        return ESP_FAIL;
    }
    
    ESP_LOGI(ESPNOW_TAG, "=== REINITIALIZING WIFI FOR MULTIPLAYER ===");
    uint32_t heap_before = esp_get_free_heap_size();
    ESP_LOGI(ESPNOW_TAG, "Free heap before reinit: %lu bytes", heap_before);
    
    // Reinitialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(ESPNOW_TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    
    // Reinitialize ESP-NOW
    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(ESPNOW_TAG, "esp_now_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Recreate queue and mutex
    espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    send_mutex = xSemaphoreCreateMutex();
    
    if (!espnow_queue || !send_mutex) {
        ESP_LOGE(ESPNOW_TAG, "Failed to create queue/mutex");
        return ESP_FAIL;
    }
    
    // Re-register callbacks
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    
    // Re-add saved peer
    esp_now_peer_info_t peer_info = {0};
    memcpy(peer_info.peer_addr, peer_mac, ESP_NOW_ETH_ALEN);
    peer_info.channel = ESPNOW_CHANNEL;
    peer_info.ifidx = ESP_IF_WIFI_STA;
    peer_info.encrypt = false;
    
    ret = esp_now_add_peer(&peer_info);
    if (ret != ESP_OK) {
        ESP_LOGE(ESPNOW_TAG, "Failed to re-add peer: %s", esp_err_to_name(ret));
        return ret;
    }
    
    wifi_deinitialized = false;
    callbacks_suspended = false;
    
    uint32_t heap_after = esp_get_free_heap_size();
    ESP_LOGI(ESPNOW_TAG, "Free heap after reinit: %lu bytes", heap_after);
    ESP_LOGI(ESPNOW_TAG, "Peer restored: %02X:%02X:%02X:%02X:%02X:%02X",
             peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
    ESP_LOGI(ESPNOW_TAG, "=== WIFI REINIT COMPLETE - MULTIPLAYER READY ===");
    
    return ESP_OK;
}

bool espnow_is_wifi_down(void)
{
    return wifi_deinitialized;
}
