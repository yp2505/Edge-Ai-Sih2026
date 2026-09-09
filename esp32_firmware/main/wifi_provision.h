// ============================================================================
// wifi_provision.h — WiFi captive-portal provisioning for Hey Vaani
//
// First boot (NVS empty):
//   Starts "HeyVaani-Setup" open AP → DNS server → HTTP captive portal.
//   User connects on phone/laptop, fills SSID / Password / Server IP,
//   presses "Save & Connect". Credentials written to NVS; device restarts.
//
// Normal boot (NVS has credentials):
//   Loads g_wifi_ssid / g_wifi_pass / g_server_ip from NVS and returns.
//   wifi_start() in main.cpp then performs the actual WiFi connection.
//   If WiFi repeatedly fails, wifi_keepalive_task calls wifi_provision_clear()
//   + esp_restart() to re-enter portal mode on the next boot.
//
// Usage from app_main():
//   1. nvs_flash_init();
//   2. wifi_provision_init();   ← loads NVS or blocks in portal mode
//   3. ... create tasks (wifi_keepalive_task calls wifi_start()) ...
// ============================================================================
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Buffer sizes ─────────────────────────────────────────────────────────────
#define PROV_SSID_MAX  33    // 32 chars + null
#define PROV_PASS_MAX  65    // 64 chars + null
#define PROV_IP_MAX    16    // "xxx.xxx.xxx.xxx\0"

// ─── Credential store ─────────────────────────────────────────────────────────
// Populated by wifi_provision_init().  Use these everywhere instead of the
// old hardcoded CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD / CONFIG_SERVER_IP.
extern char g_wifi_ssid[PROV_SSID_MAX];
extern char g_wifi_pass[PROV_PASS_MAX];
extern char g_server_ip[PROV_IP_MAX];

// ─── API ──────────────────────────────────────────────────────────────────────

/**
 * Call once from app_main(), immediately after nvs_flash_init() and before
 * any task creation.
 *
 * Behaviour:
 *   • NVS has credentials  → loads them into g_wifi_ssid / g_wifi_pass /
 *                            g_server_ip and returns immediately.
 *   • NVS is empty         → starts "HeyVaani-Setup" open AP, a DNS server
 *                            (port 53, all queries → 192.168.4.1), and an
 *                            HTTP captive portal.  Blocks until the user
 *                            submits the web form; saves to NVS; calls
 *                            esp_restart().  Never returns in this path.
 */
void wifi_provision_init(void);

/**
 * Erase all stored credentials from NVS.
 * Calling esp_restart() afterwards forces the setup portal on next boot.
 * Used by wifi_keepalive_task after repeated connection failures.
 */
void wifi_provision_clear(void);

/**
 * Update OLED screen to show Setup WiFi / Captive Portal instructions.
 */
void display_show_provisioning(void);

#ifdef __cplusplus
}
#endif
