#include "blynk_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <arpa/inet.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "w5500_net.h"

// Blynk's documented 5-byte binary frame: command, big-endian ID and length.
// Command values match the MIT-licensed Blynk protocol definitions.
enum {
    CMD_RESPONSE = 0, CMD_PING = 6, CMD_INTERNAL = 17,
    CMD_HARDWARE = 20, CMD_HW_LOGIN = 29, CMD_REDIRECT = 41,
    CMD_EVENT_LOG = 64,
};

#define BLYNK_SERVER "blynk.cloud"
#define BLYNK_PORT 443
#define HEARTBEAT_MS 45000
#define STATUS_INTERVAL_MS (5 * 60 * 1000)

static const char *TAG = "blynk";

typedef struct {
    esp_tls_t *tls;
    uint16_t next_id;
    int64_t last_in_ms;
    int64_t last_out_ms;
    int64_t wake_check_ms;
    int64_t status_check_ms;
    bool connected;
    char host[80];
    int port;
    app_config_t config;
    EventGroupHandle_t wifi_events;
    EventBits_t wifi_ready_bit;
} blynk_state_t;

static blynk_state_t state;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool socket_ready(esp_tls_t *tls, int wait_ms)
{
    if (esp_tls_get_bytes_avail(tls) > 0) return true;
    int fd;
    if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK) return false;
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(fd, &read_set);
    struct timeval timeout = {.tv_sec = wait_ms / 1000,
                              .tv_usec = (wait_ms % 1000) * 1000};
    return select(fd + 1, &read_set, NULL, NULL, &timeout) > 0;
}

static bool tls_read_exact(esp_tls_t *tls, void *buffer, size_t length)
{
    uint8_t *out = buffer;
    size_t done = 0;
    int64_t deadline = now_ms() + 5000;
    while (done < length && now_ms() < deadline) {
        if (!socket_ready(tls, 250)) continue;
        ssize_t n = esp_tls_conn_read(tls, out + done, length - done);
        if (n <= 0) return false;
        done += n;
    }
    return done == length;
}

static bool tls_write_all(esp_tls_t *tls, const void *buffer, size_t length)
{
    const uint8_t *data = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n = esp_tls_conn_write(tls, data + done, length - done);
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

static bool send_command(uint8_t command, uint16_t id, const void *body, size_t length)
{
    if (!state.tls || length > 512) return false;
    uint8_t frame[5 + 512];
    if (!id) {
        state.next_id++;
        if (!state.next_id) state.next_id++;
        id = state.next_id;
    }
    frame[0] = command;
    frame[1] = (uint8_t)(id >> 8);
    frame[2] = (uint8_t)id;
    frame[3] = (uint8_t)(length >> 8);
    frame[4] = (uint8_t)length;
    if (body && length) memcpy(frame + 5, body, length);
    if (!tls_write_all(state.tls, frame, command == CMD_RESPONSE ? 5 : 5 + length)) {
        state.connected = false;
        return false;
    }
    state.last_out_ms = now_ms();
    return true;
}

static bool send_response(uint16_t id, uint16_t status)
{
    // For response frames the length field carries the status, without a body.
    return send_command(CMD_RESPONSE, id, NULL, status);
}

static bool add_field(char *body, size_t cap, size_t *used, const char *value)
{
    size_t length = strlen(value) + 1;
    if (*used + length > cap) return false;
    memcpy(body + *used, value, length);
    *used += length;
    return true;
}

static void send_info(void)
{
    char body[256];
    size_t length = 0;
    const char *fields[] = {
        "blnkinf", "mcu", "esp-idf-6.0.3", "build", __DATE__ " " __TIME__,
        "blynk", "1.3.2", "h-beat", "45", "buff-in", "512",
        "dev", "ESP32", "con", "WiFi",
    };
    for (size_t n = 0; n < sizeof(fields) / sizeof(fields[0]); ++n) {
        if (!add_field(body, sizeof(body), &length, fields[n])) return;
    }
    send_command(CMD_INTERNAL, 0, body, length - 1);
}

static void virtual_write(int pin, const char *value)
{
    if (!state.connected) return;
    char body[256];
    char pin_text[8];
    snprintf(pin_text, sizeof(pin_text), "%d", pin);
    size_t length = 0;
    if (!add_field(body, sizeof(body), &length, "vw") ||
        !add_field(body, sizeof(body), &length, pin_text) ||
        !add_field(body, sizeof(body), &length, value)) return;
    send_command(CMD_HARDWARE, 0, body, length - 1);
}

static void log_event(const char *description)
{
    if (!state.connected) return;
    char body[160];
    size_t length = 0;
    if (!add_field(body, sizeof(body), &length, "wol_event") ||
        !add_field(body, sizeof(body), &length, description)) return;
    send_command(CMD_EVENT_LOG, 0, body, length - 1);
}

static void status_text(const char *text, bool online)
{
    virtual_write(1, online ? "255" : "0");
    char line[192];
    size_t length = strlen(text);
    if (length + 2 > sizeof(line)) return;
    memcpy(line, text, length);
    line[length++] = '\n';
    line[length] = '\0';
    virtual_write(2, line);
}

static void check_pc_status(bool after_wake)
{
    bool online = w5500_pc_online(state.config.target_ip);
    status_text(online ? "PC is online" : (after_wake ? "PC is still offline" : "PC is offline"), online);
    if (after_wake && online) log_event("PC is now online");
    ESP_LOGI(TAG, "PC %s", online ? "online" : "offline");
}

static void on_virtual_write(int pin, const char *value)
{
    if (pin == 0 && strcmp(value, "1") == 0) {
        const uint8_t unset_mac[6] = {0};
        if (memcmp(state.config.target_mac, unset_mac, sizeof(unset_mac)) == 0) {
            status_text("Set target MAC on V3 before using WoL", false);
            return;
        }
        status_text("Waking PC up...", false);
        esp_err_t err = w5500_send_magic_packet(state.config.target_mac, state.config.wol_port);
        if (err == ESP_OK) {
            status_text("Waiting for PC to wake up...", false);
            log_event("Wake-on-LAN packet sent to PC");
            state.wake_check_ms = now_ms() + 60000;
        } else {
            status_text("W5500 link down or WoL send failed", false);
            ESP_LOGW(TAG, "WoL send failed: %s", esp_err_to_name(err));
        }
    } else if (pin == 3) {
        uint8_t mac[6];
        char normalized[24];
        snprintf(normalized, sizeof(normalized), "%s", value);
        for (char *p = normalized; *p; ++p) {
            if (*p == '-') *p = ':';
            if (*p >= 'a' && *p <= 'f') *p -= 'a' - 'A';
        }
        if (app_config_parse_mac(normalized, mac) && app_config_save_target_mac(normalized)) {
            memcpy(state.config.target_mac, mac, 6);
            status_text("Target MAC saved", false);
        } else status_text("Invalid MAC: use AA:BB:CC:DD:EE:FF", false);
    } else if (pin == 4) {
        if (app_config_save_target_ip(value)) {
            snprintf(state.config.target_ip, sizeof(state.config.target_ip), "%s", value);
            status_text("Target IP saved", false);
        } else status_text("Invalid target IP", false);
    } else if (pin == 5) {
        char *end = NULL;
        long port = strtol(value, &end, 10);
        if (end != value && *end == 0 && port > 0 && port <= 65535 &&
            app_config_save_target_port((uint16_t)port)) {
            state.config.wol_port = (uint16_t)port;
            status_text("WoL port saved", false);
        } else status_text("Invalid WoL port", false);
    } else if (pin == 6 && strcmp(value, "1") == 0) {
        char info[160];
        const uint8_t *m = state.config.target_mac;
        snprintf(info, sizeof(info), "MAC: %02X:%02X:%02X:%02X:%02X:%02X\nIP: %s\nPort: %u",
                 m[0], m[1], m[2], m[3], m[4], m[5],
                 state.config.target_ip, state.config.wol_port);
        status_text(info, false);
    } else if (pin == 7 && strcmp(value, "1") == 0) {
        if (app_config_set_force_config(true)) {
            status_text("Rebooting to config mode...", false);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
}

static bool process_frame(bool *redirect)
{
    uint8_t header[5];
    if (!tls_read_exact(state.tls, header, sizeof(header))) return false;
    uint8_t command = header[0];
    uint16_t id = ((uint16_t)header[1] << 8) | header[2];
    uint16_t length = ((uint16_t)header[3] << 8) | header[4];
    state.last_in_ms = now_ms();

    if (command == CMD_RESPONSE) {
        if (id == 1 && !state.connected) {
            if (length != 200 && length != 4) {
                ESP_LOGE(TAG, "Blynk login rejected (status %u)", length);
                return false;
            }
            state.connected = true;
            ESP_LOGI(TAG, "Blynk connected via TLS");
            send_info();
            status_text("System ready", false);
            state.status_check_ms = now_ms() + STATUS_INTERVAL_MS;
        }
        return true;
    }
    if (length > 512 || id == 0) return false;
    char body[513];
    if (!tls_read_exact(state.tls, body, length)) return false;
    body[length] = 0;

    if (command == CMD_PING) return send_response(id, 200);
    if (command == CMD_REDIRECT) {
        size_t host_len = strnlen(body, length);
        if (host_len > 0 && host_len < sizeof(state.host)) {
            memcpy(state.host, body, host_len);
            state.host[host_len] = '\0';
            // TLS still uses port 443 after a cloud redirect.
            state.port = BLYNK_PORT;
            ESP_LOGI(TAG, "Blynk redirected to %s", state.host);
            *redirect = true;
        }
        return true;
    }
    if (command == CMD_HARDWARE) {
        size_t first = strnlen(body, length);
        if (first + 1 < length && strcmp(body, "vw") == 0) {
            char *pin_text = body + first + 1;
            size_t second = strnlen(pin_text, length - first - 1);
            if (first + second + 2 < length) {
                int pin = atoi(pin_text);
                const char *value = pin_text + second + 1;
                on_virtual_write(pin, value);
            }
        }
    }
    return true;
}

static void blynk_task(void *arg)
{
    (void)arg;
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));

    while (true) {
        if (!(xEventGroupWaitBits(state.wifi_events, state.wifi_ready_bit, pdFALSE,
                                  pdTRUE, pdMS_TO_TICKS(1000)) & state.wifi_ready_bit)) continue;

        time_t clock_now;
        time(&clock_now);
        if (clock_now < 1700000000) {
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) != ESP_OK) {
                ESP_LOGW(TAG, "Waiting for network time before TLS");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        esp_tls_cfg_t tls_cfg = {
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
        };
        state.tls = esp_tls_init();
        if (!state.tls) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (esp_tls_conn_new_sync(state.host, strlen(state.host), state.port,
                                  &tls_cfg, state.tls) != 1) {
            ESP_LOGW(TAG, "Blynk TLS connection failed");
            esp_tls_conn_destroy(state.tls);
            state.tls = NULL;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        state.connected = false;
        state.next_id = 1;
        state.last_in_ms = state.last_out_ms = now_ms();
        if (!send_command(CMD_HW_LOGIN, 1, state.config.blynk_auth,
                          strlen(state.config.blynk_auth))) {
            esp_tls_conn_destroy(state.tls);
            state.tls = NULL;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        bool redirect = false;
        int64_t login_deadline = now_ms() + 12000;
        while ((xEventGroupGetBits(state.wifi_events) & state.wifi_ready_bit) &&
               !redirect && now_ms() - state.last_in_ms < 120000) {
            if (!state.connected && now_ms() > login_deadline) break;
            if (socket_ready(state.tls, 200)) {
                if (!process_frame(&redirect)) break;
            }
            if (!state.connected) continue;
            int64_t now = now_ms();
            if (state.wake_check_ms && now >= state.wake_check_ms) {
                state.wake_check_ms = 0;
                check_pc_status(true);
            }
            if (now >= state.status_check_ms) {
                check_pc_status(false);
                state.status_check_ms = now_ms() + STATUS_INTERVAL_MS;
            }
            if (now - state.last_out_ms >= HEARTBEAT_MS) send_command(CMD_PING, 0, NULL, 0);
        }
        state.connected = false;
        esp_tls_conn_destroy(state.tls);
        state.tls = NULL;
        if (!redirect) vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void blynk_client_start(const app_config_t *config, EventGroupHandle_t wifi_events,
                        EventBits_t wifi_ready_bit)
{
    memset(&state, 0, sizeof(state));
    state.config = *config;
    state.wifi_events = wifi_events;
    state.wifi_ready_bit = wifi_ready_bit;
    strcpy(state.host, BLYNK_SERVER);
    state.port = BLYNK_PORT;
    xTaskCreate(blynk_task, "blynk", 10240, NULL, 5, NULL);
}
