#include "net.h"

#include <stdio.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#if CONFIG_AB_FEATURE_SNTP
#include "esp_netif_sntp.h"
#endif
#include <stdlib.h>
#include <time.h>
#include "sdkconfig.h"
#if CONFIG_AB_FEATURE_MDNS
#include "mdns.h"
#endif

static const char *TAG = "net";

static char s_eth_ip[16];    // address on the wired interface ("" = none)
static char s_wifi_ip[16];   // address on the Wi-Fi interface ("" = none)
static esp_eth_handle_t s_eth;
static volatile bool s_has_ip;   // either interface has an address

static void update_has_ip(void) { s_has_ip = s_eth_ip[0] || s_wifi_ip[0]; }

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        s_eth_ip[0] = '\0';
        update_has_ip();
        break;
    default:
        break;
    }
}

#if CONFIG_AB_FEATURE_MDNS
// Answer "audiobrick.local" and announce the services, so no fixed address is needed.
static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }
    mdns_hostname_set("audiobrick");
    mdns_instance_name_set("Esparagus Audio Brick");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
#if CONFIG_AB_FEATURE_SYNTH
    mdns_service_add(NULL, "_osc", "_udp", 9000, NULL, 0);
#endif
#if CONFIG_AB_FEATURE_VBAN
    mdns_service_add(NULL, "_vban", "_udp", 6980, NULL, 0);
#endif
    ESP_LOGI(TAG, "mDNS: audiobrick.local");
}
#endif

// Network time follows whichever interface comes up first.
static void on_network_up(void)
{
#if CONFIG_AB_FEATURE_SNTP
    static bool sntp_started;
    if (!sntp_started) {  // network time: the clock is set (and kept right) from the pool, shown in the page's status
        sntp_started = true;
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);  // Central European time; make this configurable when someone needs another zone
        tzset();
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&cfg) != ESP_OK) ESP_LOGW(TAG, "SNTP init failed");
    }
#endif
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    bool wifi = id == IP_EVENT_STA_GOT_IP;
    char *dst = wifi ? s_wifi_ip : s_eth_ip;
    snprintf(dst, 16, IPSTR, IP2STR(&ev->ip_info.ip));
    update_has_ip();
    ESP_LOGI(TAG, "got IP %s (%s)", dst, wifi ? "Wi-Fi" : "Ethernet");
    on_network_up();
}

static void on_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    s_wifi_ip[0] = '\0';
    update_has_ip();
    ESP_LOGW(TAG, "Wi-Fi address lost");
}

bool net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    spi_bus_config_t bus = {
        .miso_io_num = PIN_SPI_MISO,
        .mosi_io_num = PIN_SPI_MOSI,
        .sclk_io_num = PIN_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,
        .spics_io_num = PIN_ETH_CS,
        .queue_size = 20,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI3_HOST, &dev);
    w5500.int_gpio_num = PIN_ETH_INT;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = PIN_ETH_RST;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
    if (!mac || !phy) {
        ESP_LOGE(TAG, "W5500 driver creation failed");
        return false;
    }
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    if (esp_eth_driver_install(&cfg, &eth) != ESP_OK) {
        ESP_LOGE(TAG, "W5500 not responding on SPI (check board)");
        return false;
    }

    // The W5500 has no MAC address of its own; use the one reserved for Ethernet in eFuse.
    uint8_t addr[6];
    ESP_ERROR_CHECK(esp_read_mac(addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, addr));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(netif, "audiobrick");
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_lost_ip, NULL));
    s_eth = eth;
    ESP_ERROR_CHECK(esp_eth_start(eth));
#if CONFIG_AB_FEATURE_MDNS
    net_set_promiscuous(true);  // the W5500 filter would drop mDNS queries sent to 224.0.0.251
    mdns_start();
#endif
    ESP_LOGI(TAG, "W5500 started, MAC %02x:%02x:%02x:%02x:%02x:%02x",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return true;
}

bool net_has_ip(void)     { return s_has_ip; }
const char *net_ip_str(void) { return s_eth_ip[0] ? s_eth_ip : s_wifi_ip; }   // wired wins, as in the routing table
const char *net_eth_ip_str(void) { return s_eth_ip; }
const char *net_wifi_ip_str(void) { return s_wifi_ip; }

void *net_eth_handle(void) { return s_eth; }

// Multicast frames may be filtered out by the Ethernet chip. Ask for "all multicast" if the driver can, otherwise
// fall back to promiscuous mode. Both ioctls take a pointer to a bool, not the value.
void net_set_promiscuous(bool on)
{
    if (!s_eth) return;
#if CONFIG_AB_FEATURE_MDNS
    on = true;  // mDNS needs multicast reception at all times
#endif
    bool v = on;
    if (esp_eth_ioctl(s_eth, ETH_CMD_S_ALL_MULTICAST, &v) != ESP_OK) {
        esp_eth_ioctl(s_eth, ETH_CMD_S_PROMISCUOUS, &v);
    }
}
