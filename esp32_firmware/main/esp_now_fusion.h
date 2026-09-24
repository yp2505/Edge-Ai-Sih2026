// esp_now_fusion.h — Cross-node confidence fusion via ESP-NOW
//
// Used by 2-node 4-mic "Hey Vaani" array (SIH 2026).
// Each node broadcasts a 20-byte fusion_packet_t when local (dual-mic-averaged)
// confidence crosses DETECT_THRESHOLD.  Decision rule:
//   - local > FUSION_IMMEDIATE_THRESHOLD (0.85): trigger immediately, no peer wait.
//   - local in [DETECT_THRESHOLD, 0.85]: wait up to FUSION_WAIT_MS (100 ms) for peer.
//     If peer also elevated: corroborate and trigger.  Else: suppress.
//   - below threshold: never triggered from this path.
//
// Handoff: only the node with HIGHER confidence opens a socket and streams.
// Tie-break by NODE_ID (lower wins) so both nodes never suppress each other.
//
// NO raw audio or model data is sent — only confidence + timestamp (20 bytes).
// RAM: ~256 B globals + 2 KB task stack = <2.5 KB total overhead.
// ============================================================================

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ─── Fusion tuning constants ─────────────────────────────────────────────────
// FUSION_IMMEDIATE_THRESHOLD: confident triggers skip peer wait entirely.
// Must be > DETECT_THRESHOLD in main.cpp (0.65). Raising reduces latency for
// clear KW without affecting borderline single-node sensitivity.
#define FUSION_IMMEDIATE_THRESHOLD  0.85f

// FUSION_WAIT_MS: max time to wait for peer report on borderline hits.
// Keep short — adds latency only to borderline cases, never to confident ones.
#define FUSION_WAIT_MS              100

// FUSION_STALE_US: peer report older than this is ignored as stale.
#define FUSION_STALE_US             (300 * 1000)   // 300 ms in microseconds

// NOTE: Peer corroboration is for HANDOFF only (which of two detecting nodes
// streams). It must NEVER suppress a valid local detection below 0.85 — that
// caused the old "need 10–15 attempts" failure mode when a peer was known.

// ─── Packet structure ────────────────────────────────────────────────────────
// MUST stay ≤ 250 bytes (ESP-NOW payload limit). Currently 20 bytes.
typedef struct __attribute__((packed)) {
    uint32_t node_id;        // sender NODE_ID (1 or 2)
    float    confidence;     // dual-mic-averaged confidence at trigger time
    float    rms;            // mic RMS at trigger time (tiebreaker for handoff)
    int64_t  timestamp_us;   // esp_timer_get_time() at trigger time
} fusion_packet_t;           // total: 4 + 4 + 4 + 8 = 20 bytes

// ─── API ─────────────────────────────────────────────────────────────────────

// Initialise ESP-NOW on this node.  Must be called AFTER WiFi is started
// (WiFi + ESP-NOW share the same radio, both can coexist in STA mode).
// node_id: this node's ID (NODE_ID define).
// If no peer MAC is known yet, pass NULL → auto-discovery mode (broadcasts
// a hello packet; stores the first peer that replies).
esp_err_t esp_now_fusion_init(uint32_t node_id, const uint8_t peer_mac[6]);

// Broadcast confidence report to registered peer (or broadcast addr if no peer yet).
// Call this whenever local dual-mic-averaged confidence >= DETECT_THRESHOLD.
void esp_now_fusion_broadcast(float confidence, float rms, int64_t timestamp_us);

// Get the most recent peer report.  Returns false if no valid report exists
// or if the report is stale (older than FUSION_STALE_US).
bool esp_now_fusion_get_peer(float* out_conf, float* out_rms, int64_t* out_age_us);

// Full fusion decision function.  Call this instead of directly queuing a detect event.
// Returns true if this node should proceed to xQueueSend(detect_queue, ...).
// Logs [FUSION] with local, peer, decision, and reason on every call.
// IMPORTANT: high-confidence (> FUSION_IMMEDIATE_THRESHOLD) detections return true
// immediately with ZERO added latency — no wait, no peer check.
bool esp_now_fusion_should_trigger(float local_conf, float local_rms, uint32_t node_id);

// Handoff decision: returns true if THIS node should stream (is the winner).
// Compares local confidence against the most recent peer report.
// Tie-break: lower NODE_ID wins.
// Logs [HANDOFF] regardless of outcome.
bool esp_now_fusion_is_winner(float local_conf, float local_rms, uint32_t node_id);

// Returns true if a valid peer has been discovered (MAC registered).
bool esp_now_fusion_peer_known(void);

#ifdef __cplusplus
}
#endif
