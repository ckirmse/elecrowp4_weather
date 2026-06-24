#include <string.h>
#include <time.h>

#include "ntp_sync.h"
#include "log_util.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char * TAG = "NtpSync";

#define CONNECTED_BIT BIT0
#define FAILED_BIT    BIT1
#define MAX_RETRIES   5

static EventGroupHandle_t s_wifi_events;
static int s_retries;
static NtpStatusCb s_status_cb;

static void on_event(void * arg, esp_event_base_t base, int32_t id, void * data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        lprintf(TAG, "WiFi STA started, connecting...");
        if (s_status_cb) s_status_cb("Connecting to WiFi...");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t * e = (wifi_event_sta_disconnected_t *)data;
        if (s_retries < MAX_RETRIES) {
            lprintf(TAG, "Disconnected (reason %d), retry %d/%d", e->reason, ++s_retries, MAX_RETRIES);
            if (s_status_cb) s_status_cb("Retrying WiFi...");
            esp_wifi_connect();
        } else {
            eprintf(TAG, "Disconnected (reason %d), max retries reached", e->reason);
            if (s_status_cb) s_status_cb("WiFi connection failed");
            xEventGroupSetBits(s_wifi_events, FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t * e = (ip_event_got_ip_t *)data;
        lprintf(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        if (s_status_cb) {
            char buf[48];
            snprintf(buf, sizeof(buf), "IP address: " IPSTR, IP2STR(&e->ip_info.ip));
            s_status_cb(buf);
        }
        s_retries = 0;
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
    }
}

void ntpSyncTime(NtpStatusCb cb) {
    s_status_cb = cb;

    if (strlen(CONFIG_WIFI_SSID) == 0) {
        lprintf(TAG, "No SSID configured — skipping NTP sync");
        if (s_status_cb) s_status_cb("No WiFi configured");
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    s_wifi_events = xEventGroupCreate();
    s_retries = 0;

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, &h_ip));

    wifi_config_t wcfg = {};
    snprintf((char *)wcfg.sta.ssid,     sizeof(wcfg.sta.ssid),     "%s", CONFIG_WIFI_SSID);
    snprintf((char *)wcfg.sta.password, sizeof(wcfg.sta.password), "%s", CONFIG_WIFI_PASSWORD);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    lprintf(TAG, "Connecting to \"%s\"...", CONFIG_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, CONNECTED_BIT | FAILED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & FAILED_BIT) {
        eprintf(TAG, "WiFi connection failed after %d retries — time will not be set", MAX_RETRIES);
        goto cleanup_wifi;
    }
    if (!(bits & CONNECTED_BIT)) {
        eprintf(TAG, "WiFi connection timed out (15s) — time will not be set");
        if (s_status_cb) s_status_cb("WiFi timed out");
        goto cleanup_wifi;
    }

    {
        lprintf(TAG, "Connected; starting SNTP sync...");
        if (s_status_cb) s_status_cb("Getting time...");
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
            time_t now;
            struct tm t;
            time(&now);
            gmtime_r(&now, &t);
            lprintf(TAG, "Synced: %04d-%02d-%02d %02d:%02d:%02d UTC",
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec);
            if (s_status_cb) s_status_cb("Time synced");
        } else {
            eprintf(TAG, "SNTP sync timed out");
            if (s_status_cb) s_status_cb("Time sync failed");
        }

        esp_netif_sntp_deinit();
    }

cleanup_wifi:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h_ip);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    vEventGroupDelete(s_wifi_events);
    lprintf(TAG, "WiFi down");
}
