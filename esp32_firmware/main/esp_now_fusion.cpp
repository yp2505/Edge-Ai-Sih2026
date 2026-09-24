// esp_now_fusion.cpp — Cross-node confidence fusion via ESP-NOW
//
// SIH 2026 — Hey Vaani 2-node 4-mic array.
// See esp_now_fusion.h for protocol description.
//
// Design notes:
//   - ESP-NOW requires WiFi to be started first (STA mode).
//   - Auto-discovery: if no peer MAC given, broadcast a "hello" packet.
//     When we receive any fusion_packet_t, we register the sender as our peer.
//   - All shared state is guarded by a FreeRTOS mutex (peer_mutex).
//   - Recv callback runs in WiFi task context — keep it short.
//   - RAM: ~256 B globals + mutex (80 B) = ~336 B static.
// ============================================================================

#include "esp_now_fusion.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"     // MACSTR / MAC2STR (moved here in ESP-IDF 6.x)
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char* TAG = "FUSION";

// ─── State ───────────────────────────────────────────────────────────────────
static uint32_t           s_node_id        = 1;
static bool               s_peer_known     = false;
static uint8_t            s_peer_mac[6]    = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}; // broadcast default
static SemaphoreHandle_t  s_peer_mutex     = NULL;

// Most recent peer report (guarded by s_peer_mutex)
static float    s_peer_conf        = 0.0f;
static float    s_peer_rms         = 0.0f;
static int64_t  s_peer_timestamp   = 0;   // esp_timer_get_time() on peer at send time
static int64_t  s_peer_recv_time   = 0;   // local time we received this packet

// Broadcast MAC
static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ─── ESP-NOW send callback ─────────────────────────────────────────────
// ESP-IDF 6.x changed the send-cb first parameter from (const uint8_t* mac_addr)
// to (const wifi_tx_info_t* tx_info).  We only log on failure so we don't use it.
static void on_send(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
    // Log failures only — success is the common case
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "ESP-NOW send FAILED");
    }
}

// ─── ESP-NOW receive callback ─────────────────────────────────────────────────
// Runs in WiFi task context — must be fast, no blocking.
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int data_len) {
    if (data_len != (int)sizeof(fusion_packet_t)) return;

    fusion_packet_t pkt;
    memcpy(&pkt, data, sizeof(pkt));

    // Ignore packets from ourselves
    if (pkt.node_id == s_node_id) return;

    // Auto-discovery: if we don't have a known peer yet, register this sender
    if (!s_peer_known) {
        memcpy(s_peer_mac, info->src_addr, 6);
        esp_now_peer_info_t peer_info = {};
        memcpy(peer_info.peer_addr, info->src_addr, 6);
        peer_info.channel  = 0;    // use current WiFi channel
        peer_info.encrypt  = false;
        peer_info.ifidx    = WIFI_IF_STA;
        esp_now_add_peer(&peer_info);
        s_peer_known = true;
        ESP_LOGI(TAG, "[FUSION] Auto-discovered peer node_%lu at " MACSTR,
                 (unsigned long)pkt.node_id, MAC2STR(info->src_addr));
    }

    // Store peer report under mutex
    if (s_peer_mutex && xSemaphoreTakeFromISR(s_peer_mutex, NULL) == pdTRUE) {
        s_peer_conf       = pkt.confidence;
        s_peer_rms        = pkt.rms;
        s_peer_timestamp  = pkt.timestamp_us;
        s_peer_recv_time  = esp_timer_get_time();
        xSemaphoreGiveFromISR(s_peer_mutex, NULL);
    }
}

// ─── esp_now_fusion_init ──────────────────────────────────────────────────────
esp_err_t esp_now_fusion_init(uint32_t node_id, const uint8_t peer_mac[6]) {
    s_node_id   = node_id;
    s_peer_mutex = xSemaphoreCreateMutex();
    if (!s_peer_mutex) {
        ESP_LOGE(TAG, "Failed to create peer_mutex");
        return ESP_ERR_NO_MEM;
    }

    uint32_t heap_before = esp_get_free_heap_size();

    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init() failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_now_register_send_cb(on_send);
    esp_now_register_recv_cb(on_recv);

    // Register peer or use broadcast
    if (peer_mac != NULL) {
        memcpy(s_peer_mac, peer_mac, 6);
        esp_now_peer_info_t peer_info = {};
        memcpy(peer_info.peer_addr, s_peer_mac, 6);
        peer_info.channel = 0;
        peer_info.encrypt = false;
        peer_info.ifidx   = WIFI_IF_STA;
        ret = esp_now_add_peer(&peer_info);
        if (ret == ESP_OK) {
            s_peer_known = true;
            ESP_LOGI(TAG, "[FUSION] Peer registered: " MACSTR, MAC2STR(s_peer_mac));
        } else {
            ESP_LOGW(TAG, "[FUSION] Peer add failed (%s), falling back to auto-discovery",
                     esp_err_to_name(ret));
        }
    } else {
        // Register broadcast address so we can send discovery hellos
        esp_now_peer_info_t bcast = {};
        memcpy(bcast.peer_addr, BROADCAST_MAC, 6);
        bcast.channel = 0;
        bcast.encrypt = false;
        bcast.ifidx   = WIFI_IF_STA;
        esp_now_add_peer(&bcast);
        ESP_LOGI(TAG, "[FUSION] No peer MAC given — auto-discovery mode (broadcast hello)");
    }

    uint32_t heap_after = esp_get_free_heap_size();
    ESP_LOGI(TAG, "[FUSION] Init done: node_id=%lu heap_delta=-%lu bytes free=%lu",
             (unsigned long)node_id,
             (unsigned long)(heap_before - heap_after),
             (unsigned long)heap_after);

    return ESP_OK;
}

// ─── esp_now_fusion_broadcast ────────────────────────────────────────────────
void esp_now_fusion_broadcast(float confidence, float rms, int64_t timestamp_us) {
    fusion_packet_t pkt;
    pkt.node_id      = s_node_id;
    pkt.confidence   = confidence;
    pkt.rms          = rms;
    pkt.timestamp_us = timestamp_us;

    // Send to known peer or broadcast
    const uint8_t* dest = s_peer_known ? s_peer_mac : BROADCAST_MAC;
    esp_now_send(dest, (const uint8_t*)&pkt, sizeof(pkt));
}

// ─── esp_now_fusion_get_peer ──────────────────────────────────────────────────
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

// ─── esp_now_fusion_peer_known ────────────────────────────────────────────────
bool esp_now_fusion_peer_known(void) {
    return s_peer_known;
}

// ─── esp_now_fusion_should_trigger ───────────────────────────────────────────
// FIX (2026-09-23): Previous logic hard-suppressed any local_conf < 0.82 even
// though DETECT_THRESHOLD in main.cpp is 0.65. That mismatch silently threw
// away valid detections in [0.65, 0.82) whenever a peer was known — the main
// reason the wake word needed many attempts. Peer corroboration is now used
// only for handoff arbitration between two valid local detections, never as a
// gate that can block a single-node trigger.
//
// Decision rule:
//   - local < DETECT_THRESHOLD_FLOOR: suppress (caller should not do this)
//   - local >= FUSION_IMMEDIATE_THRESHOLD: trigger immediately (no wait)
//   - otherwise: broadcast, wait briefly for peer; if BOTH elevated → handoff
//     (higher conf wins); if peer silent/unknown → still trigger locally.
bool esp_now_fusion_should_trigger(float local_conf, float local_rms, uint32_t node_id) {
    // Floor: caller already checked DETECT_THRESHOLD in main.cpp; keep a safety net.
    static const float DETECT_THRESHOLD_FLOOR = 0.65f;

    if (local_conf < DETECT_THRESHOLD_FLOOR) {
        printf("[FUSION] local=%.4f decision=SUPPRESS reason=below_detect_floor\n",
               (double)local_conf);
        fflush(stdout);
        return false;
    }

    // Standalone (no peer discovered yet): always trigger.
    if (!s_peer_known) {
        printf("[FUSION] local=%.4f decision=TRIGGER reason=standalone_no_peer\n",
               (double)local_conf);
        fflush(stdout);
        return true;
    }

    // High-confidence path: zero latency, no peer wait.
    if (local_conf >= FUSION_IMMEDIATE_THRESHOLD) {
        esp_now_fusion_broadcast(local_conf, local_rms, esp_timer_get_time());
        printf("[FUSION] local=%.4f decision=TRIGGER reason=high_confidence_immediate\n",
               (double)local_conf);
        fflush(stdout);
        return true;
    }

    // Borderline path [0.65, 0.85): broadcast and wait briefly for peer.
    esp_now_fusion_broadcast(local_conf, local_rms, esp_timer_get_time());

    float   peer_conf  = 0.0f, peer_rms = 0.0f;
    int64_t peer_age   = 0;
    bool    peer_valid = false;
    int     waited_ms  = 0;

    while (waited_ms < FUSION_WAIT_MS) {
        peer_valid = esp_now_fusion_get_peer(&peer_conf, &peer_rms, &peer_age);
        if (peer_valid && peer_age < (int64_t)FUSION_WAIT_MS * 1000) break;
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }

    // Both nodes saw the keyword → decide which one streams (handoff).
    if (peer_valid && peer_conf >= DETECT_THRESHOLD_FLOOR) {
        bool winner = esp_now_fusion_is_winner(local_conf, local_rms, node_id);
        printf("[FUSION] local=%.4f peer=%.4f decision=%s reason=corroborated waited_ms=%d\n",
               (double)local_conf, (double)peer_conf,
               winner ? "TRIGGER" : "HANDOFF_SUPPRESS",
               waited_ms);
        fflush(stdout);
        return winner;
    }

    // Peer silent / stale / below floor → THIS node still triggers.
    // Local multi-hit debounce in main.cpp already filtered single-frame noise;
    // peer silence must not erase a valid local detection (fixes missed wakes).
    printf("[FUSION] local=%.4f peer=%.4f(valid=%d) decision=TRIGGER reason=local_only waited_ms=%d\n",
           (double)local_conf,
           peer_valid ? (double)peer_conf : 0.0,
           (int)peer_valid,
           waited_ms);
    fflush(stdout);
    return true;
}

// ─── esp_now_fusion_is_winner ─────────────────────────────────────────────────
// Handoff: true = this node streams. False = suppress, other node streams.
bool esp_now_fusion_is_winner(float local_conf, float local_rms, uint32_t node_id) {
    float   peer_conf = 0.0f, peer_rms = 0.0f;
    int64_t peer_age  = 0;
    bool    peer_valid = esp_now_fusion_get_peer(&peer_conf, &peer_rms, &peer_age);

    bool winner;
    const char* reason;

    if (!peer_valid) {
        // No peer data — we win by default (single-node mode)
        winner = true;
        reason = "no_peer_data";
    } else if (local_conf > peer_conf + 0.01f) {
        // Clearly higher confidence — win
        winner = true;
        reason = "higher_confidence";
    } else if (peer_conf > local_conf + 0.01f) {
        // Peer clearly higher — suppress
        winner = false;
        reason = "peer_higher_confidence";
    } else if (local_rms > peer_rms) {
        // Tie on confidence — use RMS as tiebreaker (better SNR wins)
        winner = true;
        reason = "rms_tiebreak";
    } else if (peer_rms > local_rms) {
        winner = false;
        reason = "peer_rms_tiebreak";
    } else {
        // Perfect tie — lower NODE_ID wins to prevent both suppressing
        winner = (node_id < /* peer node_id */ (node_id == 1 ? 2u : 1u));
        reason = "node_id_tiebreak";
    }

    if (winner) {
        printf("[HANDOFF] winner=node_%lu confidence=%.4f rms=%.4f peer_conf=%.4f reason=%s\n",
               (unsigned long)node_id, (double)local_conf, (double)local_rms,
               peer_valid ? (double)peer_conf : 0.0, reason);
    } else {
        printf("[HANDOFF] loser=node_%lu suppressed winner=node_%u confidence=%.4f peer_conf=%.4f reason=%s\n",
               (unsigned long)node_id,
               (node_id == 1 ? 2u : 1u),
               (double)local_conf,
               peer_valid ? (double)peer_conf : 0.0, reason);
    }
    fflush(stdout);
    return winner;
}
