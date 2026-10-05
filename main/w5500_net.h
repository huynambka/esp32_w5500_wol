#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t w5500_net_start(void);
bool w5500_net_link_up(void);
esp_err_t w5500_send_magic_packet(const uint8_t mac[6], uint16_t port);
bool w5500_pc_online(const char *ip);
