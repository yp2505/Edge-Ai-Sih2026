#include "esp_now_fusion.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const char* TAG = "FUSION";

static uint32_t           s_node_id        = 1;
static bool               s_peer_known     = false;
static SemaphoreHandle_t  s_peer_mutex     = NULL;

static float    s_peer_conf        = 0.0f;
static float    s_peer_rms         = 0.0f;
static int64_t  s_peer_timestamp   = 0;
static int64_t  s_peer_recv_time   = 0;

static int s_udp_sock = -1;
static int64_t s_last_discovery_us = 0;
static int s_discovery_grace_ticks = 0;

static void udp_recv_task(void* arg) {
    uint8_t rx_buffer[128];
    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(s_udp_sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

        if (len == sizeof(fusion_packet_t)) {
            fusion_packet_t pkt;
            memcpy(&pkt, rx_buffer, sizeof(pkt));

            if (pkt.node_id == s_node_id) continue;

            bool is_discovery = (pkt.confidence == DISCOVERY_CONFIDENCE);

            if (!s_peer_known) {
                s_peer_known = true;
                ESP_LOGI(TAG, "[FUSION] Auto-discovered peer node_%lu at %s", 
                         (unsigned long)pkt.node_id, inet_ntoa(source_addr.sin_addr));
            }

            if (is_discovery) continue;

            if (s_peer_mutex && xSemaphoreTake(s_peer_mutex, portMAX_DELAY) == pdTRUE) {
                s_peer_conf       = pkt.confidence;
                s_peer_rms        = pkt.rms;
                s_peer_timestamp  = pkt.timestamp_us;
                s_peer_recv_time  = esp_timer_get_time();
                xSemaphoreGive(s_peer_mutex);
            }
        }
    }
}

esp_err_t esp_now_fusion_init(uint32_t node_id, const uint8_t peer_mac[6]) {
    s_node_id   = node_id;
    s_peer_mutex = xSemaphoreCreateMutex();

    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        return ESP_FAIL;
    }

    int broadcastEnable = 1;
    setsockopt(s_udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(18266);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_udp_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Socket unable to bind");
        close(s_udp_sock);
        s_udp_sock = -1;
        return ESP_FAIL;
    }

    xTaskCreate(udp_recv_task, "udp_recv", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "[FUSION] UDP Init done: node_id=%lu", (unsigned long)node_id);
    return ESP_OK;
}

void esp_now_fusion_broadcast(float confidence, float rms, int64_t timestamp_us) {
    if (s_udp_sock < 0) return;
    fusion_packet_t pkt;
    pkt.node_id      = s_node_id;
    pkt.confidence   = confidence;
    pkt.rms          = rms;
    pkt.timestamp_us = timestamp_us;

    struct sockaddr_in dest_addr = {};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(18266);
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");

    sendto(s_udp_sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
}

bool esp_now_fusion_get_peer(float* out_conf, float* out_rms, int64_t* out_age_us) {
    if (!s_peer_mutex) return false;
    bool valid = false;
    if (xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        int64_t now  = esp_timer_get_time();
        int64_t age  = now - s_peer_recv_time;
        if (s_peer_recv_time > 0 && age < FUSION_STALE_US) {
            if (out_conf)    *out_conf    = s_peer_conf;
            if (out_rms)     *out_rms     = s_peer_rms;
            if (out_age_us)  *out_age_us  = age;
            valid = true;
        }
        xSemaphoreGive(s_peer_mutex);
    }
    return valid;
}

bool esp_now_fusion_peer_known(void) { return s_peer_known; }

