#include "w5500_net.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

#define ETH_SPI_HOST SPI2_HOST
#define ETH_PIN_MOSI 23
#define ETH_PIN_MISO 16
#define ETH_PIN_SCLK 18
#define ETH_PIN_CS 5
#define ETH_PIN_INT 4
#define ETH_PIN_RST 19
#define ETH_IP "192.168.10.2"
#define ETH_BROADCAST "192.168.10.255"

static const char *TAG = "w5500";
static volatile bool s_link_up;

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_link_up = true;
        ESP_LOGI(TAG, "Link up: %s/24", ETH_IP);
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_link_up = false;
        ESP_LOGW(TAG, "Link down");
    }
}

esp_err_t w5500_net_start(void)
{
    esp_err_t isr_error = gpio_install_isr_service(0);
    if (isr_error != ESP_OK && isr_error != ESP_ERR_INVALID_STATE) return isr_error;

    spi_bus_config_t bus = {
        .mosi_io_num = ETH_PIN_MOSI,
        .miso_io_num = ETH_PIN_MISO,
        .sclk_io_num = ETH_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1600,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(ETH_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI init");

    spi_device_interface_config_t spi_device = {
        .mode = 0,
        .clock_speed_hz = 10 * 1000 * 1000,
        .spics_io_num = ETH_PIN_CS,
        .queue_size = 16,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &spi_device);
    w5500.base.int_gpio_num = ETH_PIN_INT;
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num = ETH_PIN_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!mac || !phy) return ESP_FAIL;

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &eth), TAG, "Ethernet install");

    uint8_t mac_addr[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac_addr, ESP_MAC_ETH), TAG, "Ethernet MAC");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, mac_addr), TAG, "Set MAC");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    if (!netif) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_dhcpc_stop(netif), TAG, "Stop Ethernet DHCP");
    esp_netif_ip_info_t ip = {0};
    esp_netif_str_to_ip4(ETH_IP, &ip.ip);
    esp_netif_str_to_ip4("255.255.255.0", &ip.netmask);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &ip), TAG, "Set Ethernet IP");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)), TAG, "Attach Ethernet");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                                   eth_event_handler, NULL), TAG, "Register Ethernet events");
    ESP_LOGI(TAG, "SPI MOSI=%d MISO=%d SCK=%d CS=%d INT=%d RST=%d",
             ETH_PIN_MOSI, ETH_PIN_MISO, ETH_PIN_SCLK, ETH_PIN_CS, ETH_PIN_INT, ETH_PIN_RST);
    return esp_eth_start(eth);
}

bool w5500_net_link_up(void)
{
    return s_link_up;
}

esp_err_t w5500_send_magic_packet(const uint8_t mac[6], uint16_t port)
{
    if (!s_link_up) return ESP_ERR_INVALID_STATE;
    uint8_t packet[102];
    memset(packet, 0xff, 6);
    for (int n = 0; n < 16; ++n) memcpy(packet + 6 + n * 6, mac, 6);

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return ESP_FAIL;
    int broadcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(4444)};
    inet_pton(AF_INET, ETH_IP, &local.sin_addr);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(fd);
        return ESP_FAIL;
    }
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, ETH_BROADCAST, &peer.sin_addr);
    ssize_t sent = sendto(fd, packet, sizeof(packet), 0,
                          (struct sockaddr *)&peer, sizeof(peer));
    close(fd);
    if (sent != sizeof(packet)) return ESP_FAIL;
    ESP_LOGI(TAG, "WoL magic packet sent to %s:%u", ETH_BROADCAST, port);
    return ESP_OK;
}

static bool try_tcp_port(const struct in_addr *ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    struct sockaddr_in local = {.sin_family = AF_INET};
    inet_pton(AF_INET, ETH_IP, &local.sin_addr);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = *ip};
    int result = connect(fd, (struct sockaddr *)&peer, sizeof(peer));
    if (result < 0 && errno == EINPROGRESS) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 600000};
        result = select(fd + 1, NULL, &write_set, NULL, &timeout);
        if (result > 0) {
            int error = 0;
            socklen_t length = sizeof(error);
            result = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0 ? 0 : -1;
        } else {
            result = -1;
        }
    }
    close(fd);
    return result == 0;
}

bool w5500_pc_online(const char *ip_text)
{
    if (!s_link_up) return false;
    struct in_addr ip;
    if (inet_pton(AF_INET, ip_text, &ip) != 1) return false;
    const uint16_t ports[] = {3389, 135, 139};
    for (size_t n = 0; n < sizeof(ports) / sizeof(ports[0]); ++n) {
        if (try_tcp_port(&ip, ports[n])) return true;
    }
    return false;
}
