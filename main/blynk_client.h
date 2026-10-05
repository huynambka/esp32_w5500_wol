#pragma once

#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

void blynk_client_start(const app_config_t *config, EventGroupHandle_t wifi_events,
                        EventBits_t wifi_ready_bit);
