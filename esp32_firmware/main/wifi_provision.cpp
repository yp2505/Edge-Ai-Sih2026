// ============================================================================
// wifi_provision.cpp — WiFi Captive-Portal Provisioning
//
// Two-path boot logic (see wifi_provision.h for overview):
//
//   Path A — credentials in NVS:
//     nvs_load_credentials() → populate globals → return to app_main()
//
//   Path B — NVS empty (first boot / factory reset):
//     prov_start_portal():
//       1. esp_wifi_start() in WIFI_MODE_AP   ("HeyVaani-Setup", open)
//       2. dns_task()  — UDP port 53, redirects all A-queries to 192.168.4.1
//       3. httpd_start()  — serves setup form at http://192.168.4.1/
//       4. Blocks forever — handle_save() saves NVS then schedules esp_restart()
// ============================================================================

#include "wifi_provision.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char* TAG = "PROV";

// ─── NVS ─────────────────────────────────────────────────────────────────────
#define NVS_NS       "vaani_cfg"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define NVS_KEY_IP   "server_ip"

// ─── AP ──────────────────────────────────────────────────────────────────────
#define AP_SSID     "HeyVaani-Setup"
#define AP_MAX_CONN 4
#define AP_IP_STR   "192.168.4.1"

// ─── Exported globals ────────────────────────────────────────────────────────
char g_wifi_ssid[PROV_SSID_MAX] = "Patel divy's S24 FE";
char g_wifi_pass[PROV_PASS_MAX] = "divypatel2712";
char g_server_ip[PROV_IP_MAX]   = "10.62.113.173";


// ─── Setup portal HTML ───────────────────────────────────────────────────────
// Served at http://192.168.4.1/ — dark-themed, responsive, minimal.
static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html>"
    "<head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Hey Vaani Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
        "background:#0d1117;color:#e6edf3;min-height:100vh;"
        "display:flex;align-items:center;justify-content:center}"
    ".card{background:#161b22;border:1px solid #30363d;"
        "border-radius:12px;padding:32px;width:90%;max-width:420px}"
    "h1{color:#7c6af7;font-size:1.5rem;margin-bottom:6px}"
    ".sub{color:#7d8590;font-size:0.85rem;margin-bottom:24px}"
    "label{display:block;color:#8b949e;font-size:0.72rem;text-transform:uppercase;"
        "letter-spacing:.06em;margin:16px 0 6px}"
    "label:first-of-type{margin-top:0}"
    "input{display:block;width:100%;background:#0d1117;border:1px solid #30363d;"
        "border-radius:8px;color:#e6edf3;padding:10px 14px;font-size:0.95rem}"
    "input:focus{outline:none;border-color:#7c6af7}"
    ".hint{color:#7d8590;font-size:0.73rem;margin-top:6px}"
    "button{width:100%;background:#7c6af7;color:#fff;border:none;"
        "border-radius:8px;padding:12px;font-size:1rem;cursor:pointer;"
        "font-weight:600;margin-top:24px;transition:background .2s}"
    "button:hover{background:#6355d8}"
    "</style></head>"
    "<body><div class='card'>"
    "<h1>&#9889; Hey Vaani</h1>"
    "<div class='sub'>Connect this device to your WiFi network</div>"
    "<form method='POST' action='/save'>"
        "<label>WiFi Network (SSID)</label>"
        "<input type='text' name='ssid' placeholder='Your network name'"
            " required autocomplete='off'>"
        "<label>WiFi Password</label>"
        "<input type='password' name='pass'"
            " placeholder='Leave blank for open networks'>"
        "<label>Server IP Address</label>"
        "<input type='text' name='ip' value='13.233.154.18'"
            " placeholder='Server IP Address' required>"
        "<div class='hint'>"
            "IP of the laptop running the Hey Vaani cloud server</div>"
        "<button type='submit'>&#128190;&nbsp;Save &amp; Connect</button>"
    "</form>"
    "</div></body></html>";

// Served after successful form submission.
static const char SAVED_HTML[] =
    "<!DOCTYPE html><html>"
    "<head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved!</title>"
    "<style>"
    "body{font-family:sans-serif;background:#0d1117;color:#e6edf3;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh}"
    ".card{background:#161b22;border:1px solid #30363d;border-radius:12px;"
        "padding:32px;max-width:420px;width:90%;text-align:center}"
    "h2{color:#3fb950;font-size:1.5rem;margin-bottom:14px}"
    "p{color:#7d8590;line-height:1.7}"
    "</style></head>"
    "<body><div class='card'>"
    "<h2>&#10003; Credentials Saved</h2>"
    "<p>Hey Vaani is connecting to your network.<br>"
       "The device restarts in 2 seconds.<br>"
       "You can reconnect to your regular WiFi now.</p>"
    "</div></body></html>";

// ─── URL helpers ─────────────────────────────────────────────────────────────
/**
 * Decode a percent-encoded URL component into dst (max_out bytes incl. null).
 * Handles %XX sequences and '+' as space.
 */
static void url_decode(const char* src, char* dst, size_t max_out) {
    size_t i = 0, j = 0;
    while (src[i] && j < max_out - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' '; i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

/**
 * Extract the value for 'key' from a URL-encoded body string.
 * E.g. parse_field("ssid=Foo&pass=Bar", "ssid", ...) → "Foo"
 */
static void parse_field(const char* body, const char* key,
                         char* out, size_t max_out) {
    char search[48];
    snprintf(search, sizeof(search), "%s=", key);
    const char* p = strstr(body, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    const char* end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char tmp[256] = {};
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, p, len); tmp[len] = '\0';
    url_decode(tmp, out, max_out);
}

// ─── NVS helpers ─────────────────────────────────────────────────────────────
/**
 * Load SSID / password / server IP from NVS into the global arrays.
 * Returns true only if SSID is non-empty (i.e. credentials were stored).
 */
static bool nvs_load_credentials(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t sl = PROV_SSID_MAX, pl = PROV_PASS_MAX, il = PROV_IP_MAX;
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, g_wifi_ssid, &sl) == ESP_OK) &&
              (nvs_get_str(h, NVS_KEY_PASS, g_wifi_pass, &pl) == ESP_OK) &&
              (nvs_get_str(h, NVS_KEY_IP,   g_server_ip, &il) == ESP_OK) &&
              (g_wifi_ssid[0] != '\0');
    nvs_close(h);
    return ok;
}

/** Save SSID / password / server IP to NVS. Returns true on success. */
static bool nvs_save_credentials(const char* ssid, const char* pass,
                                  const char* ip) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    bool ok = (nvs_set_str(h, NVS_KEY_SSID, ssid) == ESP_OK) &&
              (nvs_set_str(h, NVS_KEY_PASS, pass) == ESP_OK) &&
              (nvs_set_str(h, NVS_KEY_IP,   ip)   == ESP_OK) &&
              (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

// Public: erase all provisioning data from NVS (triggers portal on next boot).
void wifi_provision_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "NVS credentials erased — portal will open on next boot");
}

// ─── DNS server task ─────────────────────────────────────────────────────────
/**
 * Minimal RFC-1035 DNS responder running on UDP port 53.
 * Answers any A-record query with 192.168.4.1 so that captive-portal
 * detection on Android / iOS / Windows actually finds our HTTP server.
 *
 * Packet layout we build:
 *   Header (12 B): copy TX-ID, set QR+RA flags, ANCOUNT=1
 *   Question section: copied verbatim from query
 *   Answer: NAME=ptr(0x0C) | TYPE=A | CLASS=IN | TTL=60 | RDLEN=4 | IP
 */
static void dns_task(void* /*arg*/) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    {   // socket options
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct timeval tv = {2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(53);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "DNS bind failed: %d", errno);
        close(sock); vTaskDelete(NULL); return;
    }

    uint32_t ap_ip = inet_addr(AP_IP_STR);   // 192.168.4.1 in network order
    ESP_LOGI(TAG, "DNS server ready on port 53 → %s", AP_IP_STR);

    static uint8_t buf[256], resp[288];
    while (true) {
        struct sockaddr_in src = {}; socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf)-1, 0,
                          (struct sockaddr*)&src, &srclen);
        if (n < 12) continue;

        // Copy query into response buffer, then patch header
        memcpy(resp, buf, (size_t)n);
        resp[2]  = 0x81; resp[3]  = 0x80;   // flags: Response + RA
        resp[4]  = 0;    resp[5]  = 1;       // QDCOUNT = 1
        resp[6]  = 0;    resp[7]  = 1;       // ANCOUNT = 1
        resp[8]  = 0;    resp[9]  = 0;       // NSCOUNT = 0
        resp[10] = 0;    resp[11] = 0;       // ARCOUNT = 0

        // Find end of question section by scanning QNAME labels
        int pos = 12;
        while (pos < n && buf[pos]) {
            if ((buf[pos] & 0xC0) == 0xC0) { pos += 2; goto qname_done; }
            pos += 1 + (int)buf[pos];
        }
        if (pos < n && buf[pos] == 0) pos++;  // null terminator
        qname_done:
        pos += 4;   // QTYPE (2) + QCLASS (2)
        if (pos > n || pos + 16 > (int)sizeof(resp)) continue;

        // Append answer record (11 fields, 16 bytes total)
        resp[pos++] = 0xC0; resp[pos++] = 0x0C;  // NAME: pointer to QNAME
        resp[pos++] = 0x00; resp[pos++] = 0x01;  // TYPE = A
        resp[pos++] = 0x00; resp[pos++] = 0x01;  // CLASS = IN
        resp[pos++] = 0x00; resp[pos++] = 0x00;  // TTL (high word)
        resp[pos++] = 0x00; resp[pos++] = 0x3C;  // TTL = 60 s
        resp[pos++] = 0x00; resp[pos++] = 0x04;  // RDLENGTH = 4
        memcpy(resp + pos, &ap_ip, 4); pos += 4; // RDATA = 192.168.4.1

        sendto(sock, resp, (size_t)pos, 0, (struct sockaddr*)&src, srclen);
    }
    close(sock);
    vTaskDelete(NULL);
}

// ─── HTTP handlers ───────────────────────────────────────────────────────────
// Serve the main setup form.
static esp_err_t handle_root(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Restart callback used by the one-shot timer below.
static void do_restart(void* /*arg*/) { esp_restart(); }

// Handle POST /save — parse form, write NVS, show success page, restart.
static esp_err_t handle_save(httpd_req_t* req) {
    char body[512] = {};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[n] = '\0';
    ESP_LOGI(TAG, "POST /save: %s", body);

    char ssid[PROV_SSID_MAX] = {}, pass[PROV_PASS_MAX] = {}, ip[PROV_IP_MAX] = {};
    parse_field(body, "ssid", ssid, sizeof(ssid));
    parse_field(body, "pass", pass, sizeof(pass));
    parse_field(body, "ip",   ip,   sizeof(ip));

    if (!ssid[0] || !ip[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "SSID and Server IP are required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saving: SSID='%s'  server=%s", ssid, ip);
    if (!nvs_save_credentials(ssid, pass, ip)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "NVS write failed");
        return ESP_FAIL;
    }

    // Respond FIRST, then restart after 2 s so the browser gets the page.
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);

    static esp_timer_handle_t rst_timer = NULL;
    if (!rst_timer) {
        esp_timer_create_args_t ta = {};
        ta.callback = do_restart;
        ta.name     = "prov_rst";
        esp_timer_create(&ta, &rst_timer);
    }
    esp_timer_start_once(rst_timer, 2000000ULL);  // 2 s
    return ESP_OK;
}

// Redirect any captive-portal probe URL back to the setup page.
static esp_err_t handle_redirect(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP_STR "/");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

// 404 catch-all → redirect to portal (handles any unlisted path).
static esp_err_t handle_404(httpd_req_t* req, httpd_err_code_t /*err*/) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" AP_IP_STR "/");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

// ─── Portal entry point ───────────────────────────────────────────────────────
/**
 * Bring up the "HeyVaani-Setup" AP, DNS server, and HTTP captive portal.
 * This function never returns — it blocks until handle_save() fires
 * esp_restart() after the user submits valid credentials.
 */
static void prov_start_portal(void) {
    ESP_LOGI(TAG, "=== Starting HeyVaani-Setup captive portal ===");

    // ── WiFi AP ──────────────────────────────────────────────────────────────
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));

    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(AP_SSID);
    ap_cfg.ap.channel        = 6;
    ap_cfg.ap.authmode       = WIFI_AUTH_OPEN;   // open — no password needed
    ap_cfg.ap.max_connection = AP_MAX_CONN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP ready  SSID='%s'  open  http://%s/", AP_SSID, AP_IP_STR);

    // ── DNS server (captive-portal redirect) ─────────────────────────────────
    xTaskCreate(dns_task, "prov_dns", 3072, NULL, 5, NULL);

    // ── HTTP server ───────────────────────────────────────────────────────────
    httpd_config_t hcfg   = HTTPD_DEFAULT_CONFIG();
    hcfg.max_uri_handlers = 14;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &hcfg));

    // Setup form (GET /)
    {
        httpd_uri_t u = {}; u.uri="/"; u.method=HTTP_GET; u.handler=handle_root;
        httpd_register_uri_handler(server, &u);
    }
    // Save endpoint (POST /save)
    {
        httpd_uri_t u = {}; u.uri="/save"; u.method=HTTP_POST; u.handler=handle_save;
        httpd_register_uri_handler(server, &u);
    }

    // Captive-portal detection paths used by Android / iOS / Windows / macOS
    // — all redirect to our form page.
    static const char* probe_uris[] = {
        "/generate_204",             // Android (Chromium connectivity check)
        "/gen_204",                  // Android alt
        "/hotspot-detect.html",      // iOS / macOS
        "/library/test/success.html",// iOS
        "/redirect",                 // Windows NCSI
        "/ncsi.txt",                 // Windows
        "/connecttest.txt",          // Windows
        "/favicon.ico",              // browsers
    };
    for (const char* uri : probe_uris) {
        httpd_uri_t u = {}; u.uri=uri; u.method=HTTP_GET; u.handler=handle_redirect;
        httpd_register_uri_handler(server, &u);
    }

    // Any other path not listed above → redirect to portal
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_404);

    ESP_LOGI(TAG, "Captive portal active. Connect to '%s', visit http://%s/",
             AP_SSID, AP_IP_STR);

    // Block forever. handle_save() fires esp_restart() after 2 s delay.
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

// ─── Public API ──────────────────────────────────────────────────────────────
void wifi_provision_init(void) {
    if (nvs_load_credentials()) {
        ESP_LOGI(TAG, "NVS OK: SSID='%s'  server=%s", g_wifi_ssid, g_server_ip);
        return;   // wifi_start() in main.cpp handles the actual connection
    }
    // No NVS credentials stored → fallback to hardcoded default hotspot
    ESP_LOGI(TAG, "NVS empty — using default hotspot: SSID='%s' server=%s", g_wifi_ssid, g_server_ip);
    nvs_save_credentials(g_wifi_ssid, g_wifi_pass, g_server_ip);
}

