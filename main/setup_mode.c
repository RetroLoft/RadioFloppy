/*
 * WiFi setup mode. See setup_mode.h and docs/API.md (setup mode).
 *
 * The access point is WPA2 with a random 8-character key that is made for
 * every setup session and shown on the OLED, so only someone who can see
 * the device can configure it. A tiny DNS server answers every name with
 * our own address and unknown URLs redirect to the setup page, so phones
 * and laptops open it by themselves ("sign in to network").
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bootloader_random.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"

#include "api.h"
#include "buttons.h"
#include "oled.h"
#include "settings.h"
#include "setup_mode.h"
#include "step_sound.h"
#include "wifi_net.h"

#define RTC_MAGIC       0x55544553  /* "SETU" */
#define SCAN_MAX        30
#define RESTART_MS      1500        /* let the HTTP answer go out first */

/* Survives esp_restart(), not a power cycle. */
typedef struct {
    uint32_t magic;
    uint8_t request;                /* setup_reason_t, 0 = none */
    uint8_t verify;                 /* new WiFi settings: verify the join */
    uint8_t fail_reason;            /* wifi_err_reason_t of the failed join */
    uint8_t pad;
    char fail_ssid[33];
    uint32_t crc;
} rtc_state_t;

static RTC_NOINIT_ATTR rtc_state_t rtc;
static rtc_state_t boot;            /* what this boot was started with */

static char ap_ssid[33];
static char ap_key[9];
static char ap_ip[16];
static char portal_url[32];
static esp_timer_handle_t restart_timer;

/* ---- Restart requests ---------------------------------------------------- */

static uint32_t rtc_crc(const rtc_state_t *s)
{
    return esp_rom_crc32_le(0, (const uint8_t *)s, offsetof(rtc_state_t, crc));
}

static void rtc_set(uint8_t request, uint8_t verify, const char *ssid, uint8_t reason)
{
    memset(&rtc, 0, sizeof(rtc));
    rtc.magic = RTC_MAGIC;
    rtc.request = request;
    rtc.verify = verify;
    rtc.fail_reason = reason;
    snprintf(rtc.fail_ssid, sizeof(rtc.fail_ssid), "%s", ssid ? ssid : "");
    rtc.crc = rtc_crc(&rtc);
}

void setup_mode_boot(void)
{
    memset(&boot, 0, sizeof(boot));
    if (rtc.magic == RTC_MAGIC && rtc.crc == rtc_crc(&rtc) &&
        memchr(rtc.fail_ssid, 0, sizeof(rtc.fail_ssid))) {
        boot = rtc;
    }
    memset(&rtc, 0, sizeof(rtc));   /* one boot only: a reset returns to normal */
}

bool setup_mode_wanted(void)
{
    settings_t s;

    settings_get(&s);
    return boot.request != 0 || s.wifi_ssid[0] == 0;
}

bool setup_mode_verify_pending(void)
{
    return boot.verify != 0;
}

void setup_mode_verify_done(void)
{
    if (boot.verify) {
        boot.verify = 0;
        printf("WiFi: new settings work\n");
    }
}

void setup_mode_restart(setup_reason_t reason)
{
    printf("Restarting into WiFi setup mode\n");
    rtc_set(reason, 0, NULL, 0);
    esp_restart();
}

void setup_mode_join_failed(const char *ssid, uint8_t wifi_reason)
{
    printf("WiFi: could not join \"%s\" (reason %u) - back to setup mode\n", ssid, wifi_reason);
    rtc_set(SETUP_JOIN_FAILED, 0, ssid, wifi_reason);
    esp_restart();
}

static void restart_cb(void *arg)
{
    esp_restart();
}

/* Restart shortly, after the HTTP response has been sent. */
static void restart_later(void)
{
    const esp_timer_create_args_t t = { .callback = restart_cb, .name = "restart" };
    if (!restart_timer && esp_timer_create(&t, &restart_timer) == ESP_OK) {
        esp_timer_start_once(restart_timer, RESTART_MS * 1000);
    }
}

/* ---- Texts --------------------------------------------------------------- */

static const char *reason_name(setup_reason_t r)
{
    switch (r) {
    case SETUP_BUTTONS:     return "buttons";
    case SETUP_JOIN_FAILED: return "join_failed";
    default:                return "not_configured";
    }
}

static const char *join_error_name(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "not_found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "wrong_password";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "security";
    case 0:
        return "no_address";        /* joined, but no IP address (DHCP) */
    default:
        return "no_connection";
    }
}

static const char *scan_security(wifi_auth_mode_t m, bool *supported)
{
    *supported = true;
    switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WPA_PSK:         return "wpa";
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2";
    case WIFI_AUTH_WPA3_PSK:        return "wpa3";
    default:
        *supported = false;         /* WEP, enterprise, OWE, ... */
        return "unsupported";
    }
}

/* ---- Setup API ------------------------------------------------------------ */

static esp_err_t get_setup(httpd_req_t *req)
{
    settings_t s;

    settings_get(&s);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "RadioFloppy");
    cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "hostname", s.hostname);
    cJSON_AddStringToObject(root, "reason", reason_name(boot.request));
    cJSON *ap = cJSON_AddObjectToObject(root, "access_point");
    cJSON_AddStringToObject(ap, "ssid", ap_ssid);
    cJSON_AddStringToObject(ap, "ip", ap_ip);
    cJSON *w = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(w, "ssid", s.wifi_ssid);
    cJSON_AddStringToObject(w, "security", settings_security_name(s.wifi_security));
    cJSON_AddBoolToObject(w, "password_set", s.wifi_pass[0] != 0);
    if (boot.request == SETUP_JOIN_FAILED) {
        cJSON *e = cJSON_AddObjectToObject(root, "join_error");
        cJSON_AddStringToObject(e, "ssid", boot.fail_ssid);
        cJSON_AddStringToObject(e, "reason", join_error_name(boot.fail_reason));
        cJSON_AddNumberToObject(e, "code", boot.fail_reason);
    }
    cJSON_AddBoolToObject(root, "can_exit", s.wifi_ssid[0] != 0);
    return api_send_json(req, "200 OK", root);
}

static esp_err_t get_scan(httpd_req_t *req)
{
    static wifi_ap_record_t recs[SCAN_MAX];
    int n = wifi_net_scan(recs, SCAN_MAX);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        bool seen = ssid[0] == 0;               /* hidden: typed by hand */
        for (int j = 0; j < i && !seen; j++) {
            seen = strcmp(ssid, (const char *)recs[j].ssid) == 0;   /* strongest first */
        }
        if (seen) {
            continue;
        }
        bool supported;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddStringToObject(o, "security", scan_security(recs[i].authmode, &supported));
        cJSON_AddBoolToObject(o, "supported", supported);
        cJSON_AddItemToArray(arr, o);
    }
    return api_send_json(req, "200 OK", root);
}

static esp_err_t put_wifi(httpd_req_t *req)
{
    cJSON *body = api_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *jssid = cJSON_GetObjectItem(body, "ssid");
    const cJSON *jpass = cJSON_GetObjectItem(body, "password");
    const cJSON *jsec = cJSON_GetObjectItem(body, "security");
    settings_t s;
    settings_get(&s);

    const char *ssid = cJSON_IsString(jssid) ? jssid->valuestring : "";
    int sec = cJSON_IsString(jsec) ? settings_security_from_name(jsec->valuestring) : -1;
    if (ssid[0] == 0 || strlen(ssid) > SETTINGS_SSID_MAX) {
        cJSON_Delete(body);
        return api_send_error(req, "422 Unprocessable Entity", "INVALID_SSID",
                              "network name: 1 to 32 characters");
    }
    if (sec < 0) {
        cJSON_Delete(body);
        return api_send_error(req, "422 Unprocessable Entity", "INVALID_SECURITY",
                              "security: wpa2, wpa3, wpa or open");
    }
    const char *pass = cJSON_IsString(jpass) ? jpass->valuestring : "";
    /* Same network, no new password typed: keep the stored one. */
    bool keep = pass[0] == 0 && sec != WIFI_SEC_OPEN && strcmp(ssid, s.wifi_ssid) == 0 &&
                s.wifi_pass[0] != 0;
    if (!keep) {
        if (strlen(pass) > SETTINGS_PASS_MAX ||
            !settings_password_valid(pass, (wifi_security_t)sec)) {
            cJSON_Delete(body);
            return api_send_error(req, "422 Unprocessable Entity", "INVALID_PASSWORD",
                                  sec == WIFI_SEC_OPEN ? "an open network has no password"
                                  : "password: 8 to 63 characters (or 64 hex digits)");
        }
        snprintf(s.wifi_pass, sizeof(s.wifi_pass), "%s", pass);
    }
    snprintf(s.wifi_ssid, sizeof(s.wifi_ssid), "%s", ssid);
    s.wifi_security = sec;
    cJSON_Delete(body);

    esp_err_t err = settings_save(&s);
    if (err != ESP_OK) {
        return api_send_error(req, "500 Internal Server Error", "FLASH_ERROR",
                              "settings could not be saved");
    }
    printf("Setup: WiFi network \"%s\" (%s) saved - restarting\n", s.wifi_ssid,
           settings_security_name(s.wifi_security));
    rtc_set(0, 1, NULL, 0);             /* next boot: verify the join */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", s.wifi_ssid);
    cJSON_AddStringToObject(root, "hostname", s.hostname);
    cJSON_AddBoolToObject(root, "restarting", true);
    esp_err_t r = api_send_json(req, "200 OK", root);
    restart_later();
    return r;
}

static esp_err_t post_exit(httpd_req_t *req)
{
    settings_t s;

    cJSON *body = api_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    cJSON_Delete(body);
    settings_get(&s);
    if (s.wifi_ssid[0] == 0) {
        return api_send_error(req, "409 Conflict", "NOT_CONFIGURED",
                              "no WiFi network is set up yet");
    }
    printf("Setup: left without changes - restarting\n");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "restarting", true);
    esp_err_t r = api_send_json(req, "200 OK", root);
    restart_later();
    return r;
}

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

static esp_err_t get_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start - 1);
}

/* Anything else (captive portal checks of phones and laptops): to the page. */
static esp_err_t redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_sendstr(req, "RadioFloppy WiFi setup");
}

static void start_http(void)
{
    httpd_handle_t server;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = 1;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        printf("Setup: HTTP server failed to start\n");
        return;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/",                   .method = HTTP_GET,  .handler = get_page },
        { .uri = "/api/v1/setup",       .method = HTTP_GET,  .handler = get_setup },
        { .uri = "/api/v1/setup/scan",  .method = HTTP_GET,  .handler = get_scan },
        { .uri = "/api/v1/setup/wifi",  .method = HTTP_PUT,  .handler = put_wifi },
        { .uri = "/api/v1/setup/exit",  .method = HTTP_POST, .handler = post_exit },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect);
}

/* ---- DNS: every A query is answered with our own address ----------------- */

static void dns_task(void *arg)
{
    uint32_t ip = inet_addr(ap_ip);
    uint8_t buf[512];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("Setup: DNS server failed to start\n");
        vTaskDelete(NULL);
    }
    while (true) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) {
            continue;                   /* not a query with one question */
        }
        int p = 12;
        while (p < n && buf[p] != 0 && (buf[p] & 0xc0) == 0) {
            p += buf[p] + 1;            /* name labels */
        }
        if (p + 5 > n || buf[p] != 0) {
            continue;
        }
        p++;
        bool type_a = buf[p] == 0 && buf[p + 1] == 1;
        p += 4;                         /* QTYPE, QCLASS */

        buf[2] = 0x84 | (buf[2] & 0x79);    /* response, authoritative, keep opcode/RD */
        buf[3] = 0x80;                      /* recursion available, no error */
        buf[6] = 0;
        buf[7] = type_a;                    /* ANCOUNT */
        memset(buf + 8, 0, 4);              /* NSCOUNT, ARCOUNT (drop EDNS) */
        if (type_a) {
            static const uint8_t ans[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
            memcpy(buf + p, ans, sizeof(ans));
            memcpy(buf + p + sizeof(ans), &ip, 4);
            p += sizeof(ans) + 4;
        }
        sendto(sock, buf, p, 0, (struct sockaddr *)&from, flen);
    }
}

/* ---- Setup mode ------------------------------------------------------------ */

/* 8 characters without look-alikes (0/O, 1/l/I): easy to read and type. */
static void make_key(char out[9])
{
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";

    bootloader_random_enable();         /* true random before the radio runs */
    for (int i = 0; i < 8; i++) {
        out[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    bootloader_random_disable();
    out[8] = 0;
}

static void leave_setup(void)
{
    settings_t s;

    settings_get(&s);
    if (s.wifi_ssid[0] == 0) {
        printf("Buttons: no WiFi network set up yet - staying in setup mode\n");
        return;
    }
    printf("Buttons: leaving setup mode - restarting\n");
    oled_show_message("Leaving setup", "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

void setup_mode_run(void)
{
    settings_t s;
    uint8_t mac[6];

    settings_get(&s);
    step_sound_init();                  /* buzzer pin in a defined, silent state */
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* At most 21 characters: fits one line of the display. */
    snprintf(ap_ssid, sizeof(ap_ssid), "%.16s-%02X%02X", s.hostname, mac[4], mac[5]);
    make_key(ap_key);

    printf("\n========================================\n");
    printf(" RadioFloppy - WiFi setup mode\n");
    printf(" Reason: %s. Floppy emulation is OFF.\n",
           boot.request == SETUP_BUTTONS ? "both buttons held"
           : boot.request == SETUP_JOIN_FAILED ? "could not join the network"
                                               : "no WiFi network set up");
    printf("========================================\n\n");

    bool display = oled_init();
    if (wifi_net_start_ap(ap_ssid, ap_key, ap_ip) != ESP_OK) {
        if (display) {
            oled_show_lines("WiFi setup mode", "WiFi failed", "", "");
        }
        return;
    }
    snprintf(portal_url, sizeof(portal_url), "http://%s/", ap_ip);
    xTaskCreatePinnedToCore(dns_task, "dns", 3072, NULL, 4, NULL, 1);
    start_http();

    char l1[24], l2[24], l3[24];
    snprintf(l1, sizeof(l1), "%.21s", ap_ssid);
    snprintf(l2, sizeof(l2), "Key %s", ap_key);
    snprintf(l3, sizeof(l3), "%.21s", portal_url);
    if (display) {
        oled_show_lines("WiFi setup mode", l1, l2, l3);
    }
    printf("Join WiFi \"%s\" with key %s and open %s\n", ap_ssid, ap_key, portal_url);
    printf("Hold both buttons 3 s to leave setup mode without changes.\n");
    buttons_start_setup(leave_setup);
}
