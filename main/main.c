#include <string.h>

#include "app_config.h"
#include "blynk_client.h"
#include "provisioning.h"
#include "w5500_net.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#define WIFI_READY_BIT BIT0

static const char *TAG = "wol";
static EventGroupHandle_t wifi_events;
static esp_timer_handle_t reconnect_timer;
static void reconnect_wifi(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_READY_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying in 3 seconds");
        if (!esp_timer_is_active(reconnect_timer)) esp_timer_start_once(reconnect_timer, 3000000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_READY_BIT);
    }
}

static void start_station(const app_config_t *config)
{
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    esp_timer_create_args_t timer_args = {.callback = reconnect_wifi, .name = "wifi_reconnect"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &reconnect_timer));

    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, config->wifi_ssid, strlen(config->wifi_ssid));
    memcpy(wifi.sta.password, config->wifi_pass, strlen(config->wifi_pass));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_events ? ESP_OK : ESP_ERR_NO_MEM);

    app_config_t config;
    app_config_load(&config);
    ESP_LOGI(TAG, "Target %02X:%02X:%02X:%02X:%02X:%02X at %s, WoL UDP %u",
             config.target_mac[0], config.target_mac[1], config.target_mac[2],
             config.target_mac[3], config.target_mac[4], config.target_mac[5],
             config.target_ip, config.wol_port);

    esp_err_t eth_error = w5500_net_start();
    if (eth_error != ESP_OK) {
        ESP_LOGE(TAG, "W5500 failed to start: %s; Wi-Fi setup and Blynk remain available",
                 esp_err_to_name(eth_error));
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (config.force_config || !config.wifi_ssid[0] || !config.blynk_auth[0]) {
        if (config.force_config) ESP_ERROR_CHECK(app_config_set_force_config(false) ? ESP_OK : ESP_FAIL);
        ESP_ERROR_CHECK(provisioning_start(false));
        return;
    }

    start_station(&config);
    if (!(xEventGroupWaitBits(wifi_events, WIFI_READY_BIT, pdFALSE, pdTRUE,
                              pdMS_TO_TICKS(20000)) & WIFI_READY_BIT)) {
        ESP_LOGW(TAG, "Wi-Fi not ready after 20 seconds; setup portal open while station retries");
        ESP_ERROR_CHECK(provisioning_start(true));
    }

    blynk_client_start(&config, wifi_events, WIFI_READY_BIT);
}
