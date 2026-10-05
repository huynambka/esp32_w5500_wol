#include "provisioning.h"

#include <ctype.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "app_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "provision";

static const char PAGE[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WoL Setup</title><style>body{font-family:sans-serif;max-width:430px;"
    "margin:2em auto;padding:0 1em}input,button{box-sizing:border-box;width:100%;"
    "padding:10px;margin:6px 0 16px}button{background:#1976d2;color:white;border:0}"
    "</style></head><body><h2>WoL Setup</h2>"
    "<p>Nhap Wi-Fi va Blynk token. De trong truong khong muon thay doi.</p>"
    "<form method='post' action='/save'>"
    "<label>Wi-Fi SSID<input name='ssid' maxlength='32'></label>"
    "<label>Wi-Fi password<input name='pass' type='password' maxlength='64'></label>"
    "<label>Blynk Auth Token<input name='auth' maxlength='63'></label>"
    "<button>Luu va khoi dong lai</button></form></body></html>";

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = (char)tolower((unsigned char)ch);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static bool form_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t key_len = strlen(key);
    const char *entry = body;
    while (*entry) {
        const char *end = strchr(entry, '&');
        if (!end) end = entry + strlen(entry);
        if ((size_t)(end - entry) >= key_len + 1 &&
            memcmp(entry, key, key_len) == 0 && entry[key_len] == '=') {
            const char *pos = entry + key_len + 1;
            size_t used = 0;
            while (pos < end) {
                unsigned char ch = (unsigned char)*pos++;
                if (ch == '+') ch = ' ';
                else if (ch == '%' && end - pos >= 2) {
                    int hi = hex_value(pos[0]);
                    int lo = hex_value(pos[1]);
                    if (hi < 0 || lo < 0) return false;
                    ch = (unsigned char)((hi << 4) | lo);
                    pos += 2;
                }
                if (ch == 0 || used + 1 >= cap) return false;
                out[used++] = (char)ch;
            }
            out[used] = 0;
            return true;
        }
        if (!*end) break;
        entry = end + 1;
    }
    out[0] = 0;
    return true;
}

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, PAGE);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    if (req->content_len >= 512) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Form too large");
        return ESP_FAIL;
    }
    char body[512];
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) return ESP_FAIL;
        received += n;
    }
    body[received] = 0;
    char ssid[33], pass[65], auth[64];
    if (!form_value(body, "ssid", ssid, sizeof(ssid)) ||
        !form_value(body, "pass", pass, sizeof(pass)) ||
        !form_value(body, "auth", auth, sizeof(auth))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
        return ESP_FAIL;
    }
    bool changed = false;
    bool saved = true;
    if (ssid[0]) {
        saved &= app_config_save_wifi(ssid, pass);
        changed = true;
    }
    if (auth[0]) {
        saved &= app_config_save_auth(auth);
        changed = true;
    }
    if (!changed || !saved) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No valid changes saved");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><meta charset='utf-8'><body><h2>Da luu</h2>Dang khoi dong lai...</body></html>");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_sendstr(req, "");
}

static void dns_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) vTaskDelete(NULL);
    struct sockaddr_in listen_addr = {.sin_family = AF_INET, .sin_port = htons(53),
                                      .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Cannot bind captive DNS");
        close(fd);
        vTaskDelete(NULL);
    }
    uint8_t packet[512];
    while (true) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int length = recvfrom(fd, packet, sizeof(packet), 0, (struct sockaddr *)&peer, &peer_len);
        if (length < 17 || (packet[2] & 0x80) || packet[4] != 0 || packet[5] != 1) continue;
        int pos = 12;
        while (pos < length && packet[pos] != 0) {
            pos += packet[pos] + 1;
        }
        if (pos + 5 > length || pos + 21 > (int)sizeof(packet)) continue;
        int question_end = pos + 5;
        if (packet[pos + 1] != 0 || packet[pos + 2] != 1 ||
            packet[pos + 3] != 0 || packet[pos + 4] != 1) continue;
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = 1;
        memset(packet + 8, 0, 4);
        uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
        memcpy(packet + question_end, answer, sizeof(answer));
        sendto(fd, packet, question_end + sizeof(answer), 0,
               (struct sockaddr *)&peer, peer_len);
    }
}

esp_err_t provisioning_start(bool keep_station)
{
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, "WoL-Setup");
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(keep_station ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (!keep_station) ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = page_handler};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler};
    httpd_uri_t redirect = {.uri = "/*", .method = HTTP_GET, .handler = redirect_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &redirect));
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Connect to Wi-Fi WoL-Setup, then open http://192.168.4.1/");
    return ESP_OK;
}
