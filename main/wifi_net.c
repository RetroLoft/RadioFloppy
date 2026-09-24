/*
 * WiFi station for the HTTP API. See wifi_net.h.
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
#include "nvs_flash.h"

#include "wifi_net.h"

#define RECONNECT_MS    5000

static esp_netif_t *netif;
static volatile bool connected;
static char ip_str[16];
static SemaphoreHandle_t started;
static esp_timer_handle_t reconnect_timer;
static esp_err_t start_result;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (connected) {
            printf("WiFi: disconnected, retrying\n");
        }
        connected = false;
        esp_timer_stop(reconnect_timer);
        esp_timer_start_once(reconnect_timer, RECONNECT_MS * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        connected = true;
        printf("WiFi: connected to \"%s\", IP %s, API http://%s/api/v1/status\n",
               CONFIG_RADIOFLOPPY_WIFI_SSID, ip_str, ip_str);
    }
}

static void reconnect_cb(void *arg)
{
    esp_wifi_connect();
}

static esp_err_t wifi_init(void)
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
    esp_netif_set_hostname(netif, CONFIG_RADIOFLOPPY_HOSTNAME);

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, CONFIG_RADIOFLOPPY_WIFI_SSID, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, CONFIG_RADIOFLOPPY_WIFI_PASSWORD, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    return esp_wifi_start();
}

static void wifi_task(void *arg)
{
    start_result = wifi_init();
    xSemaphoreGive(started);
    vTaskDelete(NULL);
}

esp_err_t wifi_net_start(void)
{
    if (strlen(CONFIG_RADIOFLOPPY_WIFI_SSID) == 0) {
        printf("WiFi: not configured (idf.py menuconfig -> RadioFloppy) - API off\n");
        return ESP_ERR_NOT_FOUND;
    }
    started = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(wifi_task, "wifi_init", 4096, NULL, 5, NULL, 1);
    xSemaphoreTake(started, portMAX_DELAY);
    printf("WiFi: %s, joining \"%s\" (host name %s)\n",
           start_result == ESP_OK ? "started" : esp_err_to_name(start_result),
           CONFIG_RADIOFLOPPY_WIFI_SSID, CONFIG_RADIOFLOPPY_HOSTNAME);
    return start_result;
}

void wifi_net_get_status(wifi_net_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->configured = strlen(CONFIG_RADIOFLOPPY_WIFI_SSID) != 0;
    st->connected = connected;
    snprintf(st->ssid, sizeof(st->ssid), "%s", CONFIG_RADIOFLOPPY_WIFI_SSID);
    if (connected) {
        snprintf(st->ip, sizeof(st->ip), "%s", ip_str);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st->rssi = ap.rssi;
        }
    }
}
