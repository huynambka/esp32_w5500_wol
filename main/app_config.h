#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char blynk_auth[64];
    uint8_t target_mac[6];
    char target_ip[16];
    uint16_t wol_port;
    bool force_config;
} app_config_t;

void app_config_load(app_config_t *config);
bool app_config_save_wifi(const char *ssid, const char *pass);
bool app_config_save_auth(const char *auth);
bool app_config_save_target_mac(const char *mac);
bool app_config_save_target_ip(const char *ip);
bool app_config_save_target_port(uint16_t port);
bool app_config_set_force_config(bool enabled);
bool app_config_parse_mac(const char *text, uint8_t out[6]);
