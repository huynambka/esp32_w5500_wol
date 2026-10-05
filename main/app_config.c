#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "nvs.h"

static bool read_string(const char *ns, const char *key, char *out, size_t size)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t required = size;
    esp_err_t err = nvs_get_str(handle, key, out, &required);
    nvs_close(handle);
    return err == ESP_OK;
}

static bool write_string(const char *ns, const char *key, const char *value)
{
    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool app_config_parse_mac(const char *text, uint8_t out[6])
{
    unsigned values[6];
    char tail;
    if (strlen(text) != 17 ||
        sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c",
               &values[0], &values[1], &values[2], &values[3],
               &values[4], &values[5], &tail) != 6) return false;
    for (size_t i = 0; i < 6; ++i) {
        if (values[i] > 255) return false;
        out[i] = (uint8_t)values[i];
    }
    return true;
}

void app_config_load(app_config_t *config)
{
    memset(config, 0, sizeof(*config));
    const uint8_t default_mac[6] = {0};
    memcpy(config->target_mac, default_mac, sizeof(default_mac));
    strcpy(config->target_ip, "192.168.10.1");
    config->wol_port = 9;

    read_string("wifi", "ssid", config->wifi_ssid, sizeof(config->wifi_ssid));
    read_string("wifi", "pass", config->wifi_pass, sizeof(config->wifi_pass));
    read_string("blynk", "auth", config->blynk_auth, sizeof(config->blynk_auth));

    char text[32] = {0};
    if (read_string("target", "mac", text, sizeof(text))) {
        uint8_t mac[6];
        if (app_config_parse_mac(text, mac)) memcpy(config->target_mac, mac, 6);
    }
    if (read_string("target", "ip", text, sizeof(text))) {
        struct in_addr addr;
        if (strlen(text) < sizeof(config->target_ip) && inet_pton(AF_INET, text, &addr) == 1) {
            memcpy(config->target_ip, text, strlen(text) + 1);
        }
    }

    nvs_handle_t handle;
    if (nvs_open("target", NVS_READONLY, &handle) == ESP_OK) {
        uint16_t port;
        if (nvs_get_u16(handle, "port", &port) == ESP_OK && port != 0) config->wol_port = port;
        nvs_close(handle);
    }
    if (nvs_open("wifi", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t forced;
        if (nvs_get_u8(handle, "force_cfg", &forced) == ESP_OK) config->force_config = forced != 0;
        nvs_close(handle);
    }
}

bool app_config_save_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !pass || !ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) return false;
    return write_string("wifi", "ssid", ssid) && write_string("wifi", "pass", pass);
}

bool app_config_save_auth(const char *auth)
{
    return auth && auth[0] && strlen(auth) < 64 && write_string("blynk", "auth", auth);
}

bool app_config_save_target_mac(const char *mac)
{
    uint8_t parsed[6];
    return mac && app_config_parse_mac(mac, parsed) && write_string("target", "mac", mac);
}

bool app_config_save_target_ip(const char *ip)
{
    struct in_addr parsed;
    return ip && strlen(ip) < 16 && inet_pton(AF_INET, ip, &parsed) == 1 &&
           write_string("target", "ip", ip);
}

bool app_config_save_target_port(uint16_t port)
{
    if (!port) return false;
    nvs_handle_t handle;
    if (nvs_open("target", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u16(handle, "port", port);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool app_config_set_force_config(bool enabled)
{
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(handle, "force_cfg", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}
