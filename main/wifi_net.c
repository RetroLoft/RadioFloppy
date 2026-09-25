/*
 * WiFi station / setup access point. See wifi_net.h.
 *
 * The WiFi driver installs its interrupts on the core that calls
 * esp_wifi_init(), so initialisation runs in a task pinned to core 1.
 * Credentials are kept in RAM (WIFI_STORAGE_RAM): no NVS writes, i.e. no
 * internal-flash operations (which would stall the caches) while the
 * floppy emulation runs. wifi_net_start() is called before the flux stream
 * starts, so the one-time PHY calibration write happens before that.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "dhcpserver/dhcpserver.h"
#include "nvs_flash.h"

#include "settings.h"
#include "setup_mode.h"
#include "wifi_net.h"

#define RECONNECT_MS    5000

static esp_netif_t *netif;
static volatile bool connected;
static bool ap_mode;
static char ip_str[16];
static char ap_ssid[33];
static SemaphoreHandle_t started;
static esp_timer_handle_t reconnect_timer;
static esp_timer_handle_t verify_timer;
static esp_err_t start_result;
static volatile uint8_t last_reason;
static settings_t cfg_now;              /* settings used for this boot */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!ap_mode) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        last_reason = ev->reason;
        if (connected) {
            printf("WiFi: disconnected, retrying\n");
        }
        connected = false;
        if (!ap_mode) {
            esp_timer_stop(reconnect_timer);
            esp_timer_start_once(reconnect_timer, RECONNECT_MS * 1000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        connected = true;
        if (verify_timer) {
            esp_timer_stop(verify_timer);
            setup_mode_verify_done();
        }
        printf("WiFi: connected to \"%s\", IP %s, API http://%s/api/v1/status\n",
               cfg_now.wifi_ssid, ip_str, ip_str);
    }
}

static void reconnect_cb(void *arg)
{
    esp_wifi_connect();
}

static void verify_cb(void *arg)
{
    if (!connected) {
        setup_mode_join_failed(cfg_now.wifi_ssid, last_reason);    /* restarts */
    }
}

/* Driver, event loop and handlers; shared by station and setup mode. */
static esp_err_t wifi_base_init(void)
{
    const esp_timer_create_args_t targs = { .callback = reconnect_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &reconnect_timer));

    esp_err_t err = nvs_flash_init();       /* PHY calibration data */
    if (err != ESP_OK) {
        printf("WiFi: NVS not usable (%s)\n", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, cfg_now.hostname);

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    return ESP_OK;
}

static wifi_auth_mode_t threshold_for(wifi_security_t sec)
{
    switch (sec) {
    case WIFI_SEC_WPA3: return WIFI_AUTH_WPA3_PSK;
    case WIFI_SEC_WPA:  return WIFI_AUTH_WPA_PSK;
    case WIFI_SEC_OPEN: return WIFI_AUTH_OPEN;
    default:            return WIFI_AUTH_WPA2_PSK;     /* WPA2 or WPA3 */
    }
}

static esp_err_t sta_init(void)
{
    esp_err_t err = wifi_base_init();
    if (err != ESP_OK) {
        return err;
    }
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, cfg_now.wifi_ssid, strlen(cfg_now.wifi_ssid));
    memcpy(wc.sta.password, cfg_now.wifi_pass, strlen(cfg_now.wifi_pass));
    wc.sta.threshold.authmode = threshold_for(cfg_now.wifi_security);
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    return esp_wifi_start();
}

static const char *ap_pass;

static esp_err_t ap_init(void)
{
    esp_err_t err = wifi_base_init();
    if (err != ESP_OK) {
        return err;
    }
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();

    /* DHCP hands out our own address as DNS server: the setup DNS server
     * answers every name with it, so phones open the setup page. */
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(ap, &ip);
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip.ip));
    esp_netif_dns_info_t dns = { .ip.type = ESP_IPADDR_TYPE_V4 };
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    dhcps_offer_t offer_dns = OFFER_DNS;
    esp_netif_dhcps_stop(ap);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    /* DHCP option 114: tells newer phones where the sign-in page is. */
    static char portal[32];
    snprintf(portal, sizeof(portal), "http://%s/", ip_str);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           portal, strlen(portal));
    esp_netif_dhcps_start(ap);

    wifi_config_t wc = { 0 };
    memcpy(wc.ap.ssid, ap_ssid, strlen(ap_ssid));
    wc.ap.ssid_len = strlen(ap_ssid);
    memcpy(wc.ap.password, ap_pass, strlen(ap_pass));
    wc.ap.channel = 6;
    wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));     /* STA only for scans */
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    return esp_wifi_start();
}

static void wifi_task(void *arg)
{
    start_result = ap_mode ? ap_init() : sta_init();
    xSemaphoreGive(started);
    vTaskDelete(NULL);
}

static esp_err_t run_init(void)
{
    started = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(wifi_task, "wifi_init", 4096, NULL, 5, NULL, 1);
    xSemaphoreTake(started, portMAX_DELAY);
    return start_result;
}

esp_err_t wifi_net_start(uint32_t verify_ms)
{
    settings_get(&cfg_now);
    if (cfg_now.wifi_ssid[0] == 0) {
        printf("WiFi: no network configured - API off\n");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = run_init();
    printf("WiFi: %s, joining \"%s\" (host name %s)\n",
           err == ESP_OK ? "started" : esp_err_to_name(err), cfg_now.wifi_ssid,
           cfg_now.hostname);
    if (err == ESP_OK && verify_ms) {
        const esp_timer_create_args_t v = { .callback = verify_cb, .name = "wifi_verify" };
        if (esp_timer_create(&v, &verify_timer) == ESP_OK) {
            esp_timer_start_once(verify_timer, (uint64_t)verify_ms * 1000);
            printf("WiFi: new settings - must connect within %lu s\n",
                   (unsigned long)(verify_ms / 1000));
        }
    }
    return err;
}

esp_err_t wifi_net_start_ap(const char *ssid, const char *pass, char ip_out[16])
{
    settings_get(&cfg_now);
    ap_mode = true;
    snprintf(ap_ssid, sizeof(ap_ssid), "%s", ssid);
    ap_pass = pass;
    esp_err_t err = run_init();
    snprintf(ip_out, 16, "%s", ip_str);
    printf("WiFi: setup access point \"%s\" %s, http://%s/\n", ap_ssid,
           err == ESP_OK ? "started" : esp_err_to_name(err), ip_str);
    return err;
}

int wifi_net_scan(wifi_ap_record_t *out, int max)
{
    uint16_t n = max;

    if (!ap_mode || esp_wifi_scan_start(NULL, true) != ESP_OK) {
        return 0;
    }
    if (esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return 0;
    }
    return n;
}

void wifi_net_get_status(wifi_net_status_t *st)
{
    settings_t s;

    memset(st, 0, sizeof(*st));
    settings_get(&s);
    st->ap_mode = ap_mode;
    st->configured = cfg_now.wifi_ssid[0] != 0 || s.wifi_ssid[0] != 0;
    st->connected = connected;
    /* The name in use since start-up (a newly saved one needs a restart). */
    snprintf(st->hostname, sizeof(st->hostname), "%s",
             cfg_now.hostname[0] ? cfg_now.hostname : s.hostname);
    if (ap_mode) {
        snprintf(st->ssid, sizeof(st->ssid), "%s", ap_ssid);
        snprintf(st->ip, sizeof(st->ip), "%s", ip_str);
        return;
    }
    snprintf(st->ssid, sizeof(st->ssid), "%s", cfg_now.wifi_ssid);
    if (connected) {
        snprintf(st->ip, sizeof(st->ip), "%s", ip_str);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st->rssi = ap.rssi;
        }
    }
}
