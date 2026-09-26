// esp_now_fusion.cpp — Cross-node confidence fusion via ESP-NOW
//
// SIH 2026 — Hey Vaani 2-node 4-mic array.
// See esp_now_fusion.h for protocol description.
//
// Design notes:
//   - ESP-NOW requires WiFi to be started first (STA mode).
//   - Auto-discovery: broadcasts a "hello" (confidence = DISCOVERY_CONFIDENCE) once
//     per FUSION_DISCOVERY_INTERVAL_MS until a sibling replies with any fusion_packet_t.
//     The first received packet's source MAC is registered as our peer.
//   - All shared state is guarded by a FreeRTOS mutex (s_peer_mutex).
//   - Recv callback runs in WiFi task context — kept short; no malloc, no heavy log.
//   - RAM: ~256 B globals + mutex (80 B) ≈ 336 B static.
//
// BUG-FIX LOG (2026-09-26):
//   [BUG-1] Previous impl used a raw UDP socket instead of the esp_now_* API.
//           ESP-NOW and UDP both use Wi-Fi but are completely different protocols.
//           Discovery never worked: (a) no esp_now_init() call, (b) no peer
//           registered before sending, (c) recv callback was never invoked.
//   [BUG-2] s_peer_known was written from the recv callback without holding the mutex,
//           causing a data race with esp_now_fusion_peer_known() readers.
//   [BUG-3] Discovery interval was checked with a tick counter but
//           s_last_discovery_us was never updated, so broadcast fired every tick.
//   [BUG-4] esp_now_fusion_get_source_zone() was a stub — always returned UNKNOWN.
//           Fully implemented with dB comparison + calibration offset.
//   [BUG-5] Node-id tiebreak in esp_now_fusion_is_winner() computed peer_node_id as
//           (node_id == 1 ? 2u : 1u) — always opposite of node_id. Making
//           "lower wins" always true for node 1 and always false for node 2.
//           Fixed: use actual peer_node_id from stored packet.
//   [BUG-6] s_peer_node_id was never stored, so tiebreak had no access to it.
//           Added s_peer_node_id global written under mutex.
// ============================================================================

#include "esp_now_fusion.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>
#include "wifi_provision.h" // For NVS-backed g_espnow_pmk and g_espnow_lmk

static const char* TAG = "FUSION";

// ─── Module state ─────────────────────────────────────────────────────────────
static uint32_t          s_node_id     = 1;
static SemaphoreHandle_t s_peer_mutex  = NULL;

// Written ONLY under s_peer_mutex:
static bool     s_peer_known     = false;
static uint8_t  s_peer_mac[6]    = {0};
static uint32_t s_peer_node_id   = 0;   // [FIX-6] actual remote node_id
static float    s_peer_conf      = 0.0f;
static float    s_peer_rms       = 0.0f;
static int64_t  s_peer_timestamp = 0;
static int64_t  s_peer_recv_time = 0;

static int64_t  s_last_discovery_us = 0;  // [FIX-3] properly gated

// ─── Helpers ──────────────────────────────────────────────────────────────────
// Register a peer MAC with ESP-NOW (idempotent).
// Sets peer.encrypt = true + LMK so ESP-NOW hardware AES-128 is active.
// Broadcast peer (FF:FF:...) is always unencrypted (HW limitation).
static void register_peer_if_needed(const uint8_t mac[6]) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;

    // Broadcast MAC cannot use encryption — ESP-NOW HW limitation.
    bool is_broadcast = (mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
                         mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF);
    if (is_broadcast) {
        peer.encrypt = false;
    } else {
        peer.encrypt = true;
        memcpy(peer.lmk, g_espnow_lmk, 16);  // LMK enables per-peer AES-128
    }

    esp_err_t e = esp_now_add_peer(&peer);
    if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "[FUSION] esp_now_add_peer failed: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "[FUSION] Peer registered: %02X:%02X:%02X:%02X:%02X:%02X encrypt=%s",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 is_broadcast ? "no(bcast)" : "yes(LMK)");
    }
}

// ─── ESP-NOW receive callback ──────────────────────────────────────────────────
// [FIX-1] Real ESP-NOW recv callback — replaces the old UDP task entirely.
// Runs in WiFi task context; must not block.
#if ESP_IDF_VERSION_MAJOR >= 5
static void on_data_recv(const esp_now_recv_info_t* recv_info,
                         const uint8_t* data, int len)
{
    const uint8_t* src_mac = recv_info->src_addr;
#else
static void on_data_recv(const uint8_t* src_mac,
                         const uint8_t* data, int len)
{
#endif
    if ((size_t)len != sizeof(fusion_packet_t)) return;

    fusion_packet_t pkt;
    memcpy(&pkt, data, sizeof(pkt));
    if (pkt.node_id == s_node_id) return;  // own broadcast echo

    bool is_discovery = (pkt.confidence == DISCOVERY_CONFIDENCE);

    if (!s_peer_mutex) return;
    // [FIX-2] Use non-blocking take — must not stall WiFi task.
    if (xSemaphoreTake(s_peer_mutex, 0) != pdTRUE) return;

    if (!s_peer_known) {
        s_peer_known = true;
        memcpy(s_peer_mac, src_mac, 6);
        s_peer_node_id = pkt.node_id;  // [FIX-6]
        // Register so subsequent sends go unicast.
        register_peer_if_needed(src_mac);
        ESP_EARLY_LOGI(TAG,
            "[FUSION] Auto-discovered peer node_%lu %02X:%02X:%02X:%02X:%02X:%02X",
            (unsigned long)pkt.node_id,
            src_mac[0], src_mac[1], src_mac[2],
            src_mac[3], src_mac[4], src_mac[5]);
    }

    if (!is_discovery) {
        s_peer_conf      = pkt.confidence;
        s_peer_rms       = pkt.rms;
        s_peer_timestamp = pkt.timestamp_us;
        s_peer_recv_time = esp_timer_get_time();
        s_peer_node_id   = pkt.node_id;  // refresh
        ESP_EARLY_LOGI(TAG, "[FUSION-RECV] From node_%lu: conf=%.4f rms=%.4f",
                       (unsigned long)pkt.node_id, (double)pkt.confidence, (double)pkt.rms);
    }

    xSemaphoreGive(s_peer_mutex);
}

#include "esp_idf_version.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_data_sent(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS && tx_info != NULL) {
        ESP_LOGD(TAG, "[FUSION] send FAIL to %02X:%02X:%02X:%02X:%02X:%02X",
                 tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
                 tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5]);
    }
}
#else
static void on_data_sent(const uint8_t* mac, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS && mac != NULL) {
        ESP_LOGD(TAG, "[FUSION] send FAIL to %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}
#endif

// ─── Public API ───────────────────────────────────────────────────────────────

esp_err_t esp_now_fusion_init(uint32_t node_id, const uint8_t peer_mac[6]) {
    s_node_id    = node_id;
    s_peer_mutex = xSemaphoreCreateMutex();
    if (!s_peer_mutex) {
        ESP_LOGE(TAG, "[FUSION] Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // [FIX-1] Initialise the ESP-NOW subsystem (was missing entirely).
    esp_err_t e = esp_now_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "[FUSION] esp_now_init failed: %s", esp_err_to_name(e));
        return e;
    }

    // Set network-wide PMK so unicast peers can use hardware AES-128 encryption.
    e = esp_now_set_pmk(g_espnow_pmk);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "[FUSION] esp_now_set_pmk failed: %s — unicast will be unencrypted",
                 esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "[FUSION] PMK set — unicast peer frames will use AES-128");
    }

    esp_now_register_recv_cb(on_data_recv);
    esp_now_register_send_cb(on_data_sent);

    if (peer_mac) {
        // Static peer provided — register immediately and mark as known.
        register_peer_if_needed(peer_mac);
        if (xSemaphoreTake(s_peer_mutex, portMAX_DELAY) == pdTRUE) {
            s_peer_known = true;
            memcpy(s_peer_mac, peer_mac, 6);
            xSemaphoreGive(s_peer_mutex);
        }
    } else {
        // Auto-discovery mode: add broadcast peer so hello packets can go out.
        static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        register_peer_if_needed(bcast);
    }

    ESP_LOGI(TAG, "[FUSION] ESP-NOW Init done: node_id=%lu mode=%s",
             (unsigned long)node_id, peer_mac ? "static_peer" : "auto-discovery");
    return ESP_OK;
}

// ─── esp_now_fusion_broadcast ─────────────────────────────────────────────────
// Sends unicast to peer if known, otherwise broadcasts.
void esp_now_fusion_broadcast(float confidence, float rms, int64_t timestamp_us) {
    fusion_packet_t pkt;
    pkt.node_id      = s_node_id;
    pkt.confidence   = confidence;
    pkt.rms          = rms;
    pkt.timestamp_us = timestamp_us;

    uint8_t dest[6];
    bool use_unicast = false;
    
    // Always broadcast discovery packets. Only unicast real confidence data.
    // If we unicast discovery, a rebooted peer can't decrypt it because it hasn't registered us yet!
    if (confidence != DISCOVERY_CONFIDENCE && s_peer_mutex && xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        use_unicast = s_peer_known;
        if (use_unicast) memcpy(dest, s_peer_mac, 6);
        xSemaphoreGive(s_peer_mutex);
    }
    
    if (!use_unicast) {
        static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        memcpy(dest, bcast, 6);
    }

    esp_err_t e = esp_now_send(dest, (const uint8_t*)&pkt, sizeof(pkt));
    if (e != ESP_OK) {
        ESP_LOGD(TAG, "[FUSION] esp_now_send: %s", esp_err_to_name(e));
    }
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

// [FIX-2] Read peer_known under mutex — was an unguarded plain read before.
bool esp_now_fusion_peer_known(void) {
    if (!s_peer_mutex) return false;
    bool known = false;
    if (xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        known = s_peer_known;
        xSemaphoreGive(s_peer_mutex);
    }
    return known;
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
bool esp_now_fusion_should_trigger(float local_conf, float local_rms, uint32_t node_id,
                                   esp_now_fusion_latency_path_t* out_latency_path) {
    static const float DETECT_THRESHOLD_FLOOR = 0.65f;

    if (local_conf < DETECT_THRESHOLD_FLOOR) {
        printf("[FUSION] local=%.4f decision=SUPPRESS reason=below_detect_floor\n",
               (double)local_conf);
        fflush(stdout);
        return false;
    }

    // [FIX-2] Use thread-safe accessor instead of bare s_peer_known read.
    bool peer_known_now = esp_now_fusion_peer_known();

    // Standalone (no peer discovered yet): always trigger immediately.
    if (!peer_known_now) {
        printf("[FUSION] local=%.4f decision=TRIGGER reason=standalone_no_peer\n",
               (double)local_conf);
        fflush(stdout);
        if (out_latency_path) *out_latency_path = ESP_NOW_FUSION_LATENCY_CONFIDENT;
        return true;
    }

    // Always broadcast detection to peer
    esp_now_fusion_broadcast(local_conf, local_rms, esp_timer_get_time());

    // Wait briefly (up to FUSION_WAIT_MS) for peer packet to arrive over ESP-NOW
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

    // Both nodes saw the keyword → perform spatial & confidence arbitration (winner streams, loser yields)
    if (peer_valid && peer_conf >= DETECT_THRESHOLD_FLOOR) {
        bool winner = esp_now_fusion_is_winner(local_conf, local_rms, node_id);
        printf("[FUSION] local=%.4f peer=%.4f decision=%s reason=corroborated waited_ms=%d\n",
               (double)local_conf, (double)peer_conf,
               winner ? "TRIGGER" : "HANDOFF_SUPPRESS",
               waited_ms);
        fflush(stdout);
        if (out_latency_path) *out_latency_path = (local_conf >= FUSION_IMMEDIATE_THRESHOLD) ? ESP_NOW_FUSION_LATENCY_CONFIDENT : ESP_NOW_FUSION_LATENCY_BORDERLINE;
        return winner;
    }

    // Peer silent / stale / below floor → THIS node triggers.
    printf("[FUSION] local=%.4f peer=%.4f(valid=%d) decision=TRIGGER reason=local_only waited_ms=%d\n",
           (double)local_conf,
           peer_valid ? (double)peer_conf : 0.0,
           (int)peer_valid,
           waited_ms);
    fflush(stdout);
    if (out_latency_path) *out_latency_path = (local_conf >= FUSION_IMMEDIATE_THRESHOLD) ? ESP_NOW_FUSION_LATENCY_CONFIDENT : ESP_NOW_FUSION_LATENCY_BORDERLINE;
    return true;
}

// ─── esp_now_fusion_is_winner ─────────────────────────────────────────────────
// Handoff: true = this node streams. False = suppress, other node streams.
// [FIX-5] Old tiebreak: (node_id == 1 ? 2u : 1u) always returned the opposite
// of node_id, so "node_id < peer_id" was always TRUE for node 1 and always
// FALSE for node 2 — completely broken in a 2-node system.
// Fix: read actual s_peer_node_id stored from the received packet.
bool esp_now_fusion_is_winner(float local_conf, float local_rms, uint32_t node_id) {
    float   peer_conf = 0.0f, peer_rms = 0.0f;
    int64_t peer_age  = 0;
    bool    peer_valid = esp_now_fusion_get_peer(&peer_conf, &peer_rms, &peer_age);

    // [FIX-5/6] Read actual peer node_id from stored state.
    uint32_t peer_node_id = 0;
    if (s_peer_mutex && xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        peer_node_id = s_peer_node_id;
        xSemaphoreGive(s_peer_mutex);
    }

    bool winner;
    const char* reason;

    if (!peer_valid) {
        winner = true;
        reason = "no_peer_data";
    } else if (local_conf > peer_conf + 0.01f) {
        winner = true;
        reason = "higher_confidence";
    } else if (peer_conf > local_conf + 0.01f) {
        winner = false;
        reason = "peer_higher_confidence";
    } else {
        // Confidence tie (within 0.01): use RMS to find which node has better SNR.
        // Use a 5% relative deadband so the comparison is meaningful at ANY distance
        // (a fixed 0.5 raw-unit gap is too tight when both nodes are close together
        // — they'd both be within 0.5 raw units and always fall through to node_id,
        //  bypassing the RMS tiebreak entirely).
        float rms_ref   = (local_rms + peer_rms) * 0.5f;
        float rms_5pct  = rms_ref * 0.05f;   // 5 % of mean RMS
        if (rms_5pct < 1.0f) rms_5pct = 1.0f; // floor so we never compare noise

        if (local_rms > peer_rms + rms_5pct) {
            // This node is meaningfully louder → better SNR → stream.
            winner = true;
            reason = "rms_tiebreak_pct";
        } else if (peer_rms > local_rms + rms_5pct) {
            // Peer is meaningfully louder.
            winner = false;
            reason = "peer_rms_tiebreak_pct";
        } else {
            // True dead-heat (< 5 % RMS gap): lower NODE_ID is the deterministic
            // tiebreaker so both nodes never simultaneously suppress each other.
            winner = (peer_node_id == 0) || (node_id < peer_node_id);
            reason = "node_id_tiebreak";
        }
    }

    if (winner) {
        printf("[HANDOFF] winner=node_%lu conf=%.4f rms=%.4f peer_conf=%.4f reason=%s\n",
               (unsigned long)node_id, (double)local_conf, (double)local_rms,
               peer_valid ? (double)peer_conf : 0.0, reason);
    } else {
        printf("[HANDOFF] loser=node_%lu suppressed peer=node_%lu conf=%.4f peer_conf=%.4f reason=%s\n",
               (unsigned long)node_id, (unsigned long)peer_node_id,
               (double)local_conf,
               peer_valid ? (double)peer_conf : 0.0, reason);
    }
    fflush(stdout);
    return winner;
}

// ─── esp_now_fusion_discovery_tick ───────────────────────────────────────────
// [FIX-3] Was calling broadcast every tick because s_last_discovery_us was
// never updated. Now properly gated by FUSION_DISCOVERY_INTERVAL_MS elapsed.
void esp_now_fusion_discovery_tick(void) {
    // Removed: if (esp_now_fusion_peer_known()) return; // Keep broadcasting 1Hz heartbeat

    int64_t now = esp_timer_get_time();
    if ((now - s_last_discovery_us) < (int64_t)FUSION_DISCOVERY_INTERVAL_MS * 1000) {
        return;  // not time yet
    }
    s_last_discovery_us = now;  // [FIX-3] actually update the timestamp
    esp_now_fusion_broadcast(DISCOVERY_CONFIDENCE, 0.0f, now);
    if (!s_peer_known) {
        ESP_LOGI(TAG, "[FUSION] Discovery hello sent (no peer yet)");
    } else {
        ESP_LOGD(TAG, "[FUSION] Heartbeat sent (peer connected)");
    }
}

// ─── esp_now_fusion_get_source_zone ──────────────────────────────────────────
// [FIX-4] Was a stub — always returned UNKNOWN. Now fully implemented.
//
// Algorithm:
//   delta_dB = 20*log10(local_rms / peer_rms) + cal_offset
//   delta_dB > +FUSION_SOURCE_CENTER_DB  → source on THIS node's side
//   delta_dB < -FUSION_SOURCE_CENTER_DB  → source on PEER node's side
//   |delta_dB| <= FUSION_SOURCE_CENTER_DB → centered
//
// FUSION_NODE1_RMS_CAL_DB compensates fixed hardware gain difference between
// the two nodes (measured at centre-position calibration, default 0 dB).
esp_now_fusion_source_zone_t esp_now_fusion_get_source_zone(
    float local_rms, uint32_t node_id,
    float* out_delta_db, uint32_t* out_peer_age_ms)
{
    if (out_delta_db)    *out_delta_db    = 0.0f;
    if (out_peer_age_ms) *out_peer_age_ms = 0;

    float   peer_rms  = 0.0f;
    float   peer_conf = 0.0f;
    int64_t peer_age  = 0;
    if (!esp_now_fusion_get_peer(&peer_conf, &peer_rms, &peer_age)) {
        return ESP_NOW_FUSION_SOURCE_UNKNOWN;
    }
    if (peer_age > FUSION_SOURCE_MAX_PEER_AGE_US) {
        return ESP_NOW_FUSION_SOURCE_UNKNOWN;  // stale — different utterance
    }
    if (out_peer_age_ms) *out_peer_age_ms = (uint32_t)(peer_age / 1000);

    // Guard against log(0) / log(negative).
    if (local_rms < 1.0f || peer_rms < 1.0f) {
        return ESP_NOW_FUSION_SOURCE_UNKNOWN;
    }

    float delta_db = 20.0f * log10f(local_rms / peer_rms);
    // Apply calibration: Node 1 correction is positive if Node 1 mic is quieter.
    if (node_id == 1) {
        delta_db += FUSION_NODE1_RMS_CAL_DB;
    } else {
        delta_db -= FUSION_NODE1_RMS_CAL_DB;  // symmetric for node 2
    }
    if (out_delta_db) *out_delta_db = delta_db;

    esp_now_fusion_source_zone_t zone;
    if (delta_db > FUSION_SOURCE_CENTER_DB) {
        // Local louder → speaker is closer to THIS node.
        zone = (node_id == 1) ? ESP_NOW_FUSION_SOURCE_NODE_1_SIDE
                              : ESP_NOW_FUSION_SOURCE_NODE_2_SIDE;
    } else if (delta_db < -FUSION_SOURCE_CENTER_DB) {
        // Peer louder → speaker is closer to PEER node.
        zone = (node_id == 1) ? ESP_NOW_FUSION_SOURCE_NODE_2_SIDE
                              : ESP_NOW_FUSION_SOURCE_NODE_1_SIDE;
    } else {
        zone = ESP_NOW_FUSION_SOURCE_CENTER;
    }
    return zone;
}

// ─── esp_now_fusion_source_zone_name ─────────────────────────────────────────
const char* esp_now_fusion_source_zone_name(esp_now_fusion_source_zone_t zone) {
    switch (zone) {
        case ESP_NOW_FUSION_SOURCE_UNKNOWN:     return "UNKNOWN";
        case ESP_NOW_FUSION_SOURCE_NODE_1_SIDE: return "NODE_1";
        case ESP_NOW_FUSION_SOURCE_CENTER:      return "CENTER";
        case ESP_NOW_FUSION_SOURCE_NODE_2_SIDE: return "NODE_2";
        default: return "UNKNOWN";
    }
}
