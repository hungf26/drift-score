#include <stdio.h>
#include <string.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Định nghĩa
#define ESPNOW_CHANNEL 1  // Channel chung
#define MAX_PEERS 10      // Giới hạn cho mạng của bạn
#define MY_ID 1           // ID duy nhất cho mỗi thiết bị (thay đổi từ 1-10)

// MAC broadcast
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Struct message
typedef enum {
    MSG_DISCOVERY,
    MSG_DATA,
    MSG_ACK
} message_type_t;

typedef struct {
    message_type_t type;
    uint8_t sender_id;
    uint8_t sender_mac[ESP_NOW_ETH_ALEN];
    float data;  // Dữ liệu mẫu, ví dụ nhiệt độ
} espnow_message_t;

// Danh sách peer (MAC và ID)
typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    uint8_t id;
} peer_info_t;
peer_info_t peers[MAX_PEERS];
int peer_count = 0;

// Callback nhận dữ liệu
static void recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (len != sizeof(espnow_message_t)) return;

    espnow_message_t msg;
    memcpy(&msg, data, sizeof(msg));

    if (msg.type == MSG_DISCOVERY) {
        // Kiểm tra nếu peer đã tồn tại
        bool exists = false;
        for (int i = 0; i < peer_count; i++) {
            if (memcmp(peers[i].mac, msg.sender_mac, ESP_NOW_ETH_ALEN) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists && peer_count < MAX_PEERS) {
            // Thêm peer mới
            memcpy(peers[peer_count].mac, msg.sender_mac, ESP_NOW_ETH_ALEN);
            peers[peer_count].id = msg.sender_id;
            esp_now_peer_info_t peer;
            memset(&peer, 0, sizeof(peer));
            memcpy(peer.peer_addr, msg.sender_mac, ESP_NOW_ETH_ALEN);
            peer.channel = ESPNOW_CHANNEL;
            peer.ifidx = WIFI_IF_STA;
            peer.encrypt = false;  // Không mã hóa để đơn giản
            if (esp_now_add_peer(&peer) == ESP_OK) {
                peer_count++;
                printf("Added peer ID %d with MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                       msg.sender_id, peer.peer_addr[0], peer.peer_addr[1], peer.peer_addr[2],
                       peer.peer_addr[3], peer.peer_addr[4], peer.peer_addr[5]);
            }
            // Gửi ACK unicast để xác nhận
            espnow_message_t ack = {MSG_ACK, MY_ID, {0}, 0};
            esp_wifi_get_mac(WIFI_IF_STA, ack.sender_mac);
            esp_now_send(msg.sender_mac, (uint8_t *)&ack, sizeof(ack));
        }
    } else if (msg.type == MSG_DATA) {
        // Xử lý dữ liệu nhận được
        printf("Received data %f from ID %d\n", msg.data, msg.sender_id);
        // Có thể forward đến các peer khác nếu cần mesh-like
    } else if (msg.type == MSG_ACK) {
        printf("Received ACK from ID %d\n", msg.sender_id);
    }
}

// Callback gửi (kiểm tra thành công)
// Callback gửi (đã cập nhật cho v5.5)
static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        printf("Send failed to MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
               tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
               tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5]);
    }
}

// Task discovery định kỳ (mỗi 10s gửi broadcast)
static void discovery_task(void *pvParameter) {
    espnow_message_t msg = {MSG_DISCOVERY, MY_ID, {0}, 0};
    esp_wifi_get_mac(WIFI_IF_STA, msg.sender_mac);

    while (1) {
        esp_now_send(broadcast_mac, (uint8_t *)&msg, sizeof(msg));
        vTaskDelay(10000 / portTICK_PERIOD_MS);  // 10 giây
    }
}

// Task gửi dữ liệu (ví dụ)
static void data_task(void *pvParameter) {
    while (1) {
        if (peer_count > 0) {
            espnow_message_t msg = {MSG_DATA, MY_ID, {0}, 25.5};  // Dữ liệu mẫu
            esp_wifi_get_mac(WIFI_IF_STA, msg.sender_mac);
            // Gửi đến tất cả peer (hoặc chọn cụ thể)
            for (int i = 0; i < peer_count; i++) {
                esp_now_send(peers[i].mac, (uint8_t *)&msg, sizeof(msg));
            }
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);  // 5 giây
    }
}

void app_main(void) {
    // Khởi tạo NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Khởi tạo Wi-Fi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    // Khởi tạo ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    // Thêm broadcast peer nếu cần (cho broadcast an toàn)
    esp_now_peer_info_t broadcast_peer;
    memset(&broadcast_peer, 0, sizeof(broadcast_peer));
    memcpy(broadcast_peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    broadcast_peer.channel = ESPNOW_CHANNEL;
    broadcast_peer.ifidx = WIFI_IF_STA;
    broadcast_peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&broadcast_peer));

    // Tạo task
    xTaskCreate(discovery_task, "discovery", 4096, NULL, 5, NULL);
    xTaskCreate(data_task, "data", 4096, NULL, 5, NULL);
}