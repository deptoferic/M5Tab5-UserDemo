/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <vector>
#include <memory>
#include <string.h>
#include <bsp/m5stack_tab5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_wifi_default.h>
#include <esp_wifi_netif.h>
#include <esp_private/wifi.h>
#include <esp_http_server.h>
#include "sensor_net/sensor_net.h"

#define TAG "wifi"

#define WIFI_SSID    "M5Tab5-UserDemo-WiFi"
#define WIFI_PASS    ""
/* Raised from M5's 4. Five sensor nodes plus a phone is six, and the ceiling
 * had to be known before designing to it (CLAUDE.md §2.5). ESP-IDF caps
 * max_connection at ESP_WIFI_MAX_CONN_NUM; the value that actually binds is the
 * one compiled into the C6's ESP-Hosted SLAVE firmware, not this line, so this
 * is a REQUEST and the accepted value is read back with `net` rather than
 * assumed. */
#define MAX_STA_CONN 10

static bool s_wifi_ap_netif_started = false;

static void wifi_remote_ap_start_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (s_wifi_ap_netif_started || esp_netif_is_netif_up(netif)) {
        ESP_LOGW(TAG, "ignore duplicate Wi-Fi AP start event");
        return;
    }

    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));
    uint8_t mac[6];
    esp_err_t ret = esp_wifi_get_if_mac(driver, mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_if_mac failed: %s", esp_err_to_name(ret));
        return;
    }

    if (esp_wifi_is_if_ready_when_started(driver)) {
        ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_register_if_rxcb failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ret = esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref, esp_netif_netstack_buf_free);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "netstack cb register failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_netif_set_mac(netif, mac);
    esp_netif_action_start(netif, base, event_id, data);
    s_wifi_ap_netif_started = true;
}

static void wifi_remote_ap_stop_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (!s_wifi_ap_netif_started && !esp_netif_is_netif_up(netif)) {
        ESP_LOGW(TAG, "ignore duplicate Wi-Fi AP stop event");
        return;
    }

    esp_netif_action_stop(netif, base, event_id, data);
    s_wifi_ap_netif_started = false;
}

static esp_netif_t* create_wifi_remote_ap_netif()
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    esp_netif_t* netif     = esp_netif_new(&cfg);
    assert(netif);

    ESP_ERROR_CHECK(esp_netif_attach_wifi_ap(netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, wifi_remote_ap_start_handler, netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, wifi_remote_ap_stop_handler, netif));

    return netif;
}

// HTTP 处理函数
esp_err_t hello_get_handler(httpd_req_t* req)
{
    const char* html_response = R"rawliteral(
        <!DOCTYPE html>
        <html>
        <head>
            <title>Hello</title>
            <style>
                body {
                    display: flex;
                    flex-direction: column;
                    justify-content: center;
                    align-items: center;
                    height: 100vh;
                    margin: 0;
                    font-family: sans-serif;
                    background-color: #f0f0f0;
                }
                h1 {
                    font-size: 48px;
                    color: #333;
                    margin: 0;
                }
                p {
                    font-size: 18px;
                    color: #666;
                    margin-top: 10px;
                }
            </style>
        </head>
        <body>
            <h1>Hello World</h1>
            <p>From M5Tab5</p>
        </body>
        </html>
    )rawliteral";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// URI 路由
httpd_uri_t hello_uri = {.uri = "/", .method = HTTP_GET, .handler = hello_get_handler, .user_ctx = nullptr};

// 启动 Web Server
static httpd_handle_t s_hal_server = nullptr;

httpd_handle_t start_webserver()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* The stock 8 handlers are already spoken for by the demo page; the sensor
     * endpoints need room alongside them. */
    config.max_uri_handlers = 16;
    httpd_handle_t server = nullptr;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &hello_uri);
        /* The sensor path shares this server rather than raising a second one:
         * two servers cannot both hold port 80, and a second port would be one
         * more thing for a node to get wrong. */
        sensor_net_attach(server);
    }
    s_hal_server = server;
    return server;
}

/* C entry points, so the sensor-network code can reach the ONE proven AP
 * bring-up rather than growing a second one. The netif glue above is hand-rolled
 * because esp_netif_create_default_wifi_ap() is declared but NOT defined for
 * esp_wifi_remote — duplicating that in C would be a second path to keep
 * correct, and the two would drift. */
void wifi_init_softap();   /* defined below; the shim is placed with the server */

extern "C" void hal_wifi_start_ap_c(void)
{
    static bool started = false;
    if (started) { return; }
    started = true;
    wifi_init_softap();
    start_webserver();
}

extern "C" httpd_handle_t hal_wifi_get_server_c(void) { return s_hal_server; }

// 初始化 Wi-Fi AP 模式
void wifi_init_softap()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    create_wifi_remote_ap_netif();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), WIFI_SSID, sizeof(wifi_config.ap.ssid));
    std::strncpy(reinterpret_cast<char*>(wifi_config.ap.password), WIFI_PASS, sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len       = std::strlen(WIFI_SSID);
    wifi_config.ap.max_connection = MAX_STA_CONN;
    wifi_config.ap.authmode       = WIFI_AUTH_OPEN;

    /* THE AP CHANNEL MUST BE SET EXPLICITLY, AND IT MUST NOT BE 1.
     *
     * Measured 2026-08-28. Left zero-initialised, ap.channel lands on 1, and on
     * channel 1 the C6's ESP-Hosted softAP DOES NOT BEACON — while reporting
     * itself perfectly healthy: esp_wifi_get_mode() says AP, get_config returns
     * the SSID, the DHCP server starts on 192.168.4.1, and the boot log prints
     * "Wi-Fi AP started". Nothing anywhere reports an error. It is simply not on
     * the air, and a station 30 cm away gets NO_AP_FOUND.
     *
     * Channels 6 and 11 both work and were both seen at -1 to -3 dBm. The
     * failure is specific to channel 1 and specific to TRANSMIT: the same C6
     * happily SEES a channel-1 AP at -8 dBm, so its receiver and antenna are
     * fine. See CLAUDE.md §8.
     *
     * 6 is chosen because it is the channel the whole path was proven end to end
     * on. 11 is the alternative and is less congested here. 1 must never be used
     * until somebody explains this. */
    wifi_config.ap.channel        = 6;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi AP started. SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
}

static void wifi_ap_test_task(void* param)
{
    wifi_init_softap();
    start_webserver();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

bool HalEsp32::wifi_init()
{
    mclog::tagInfo(TAG, "wifi init");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    xTaskCreate(wifi_ap_test_task, "ap", 4096, nullptr, 5, nullptr);
    return true;
}

void HalEsp32::setExtAntennaEnable(bool enable)
{
    _ext_antenna_enable = enable;
    mclog::tagInfo(TAG, "set ext antenna enable: {}", _ext_antenna_enable);
    bsp_set_ext_antenna_enable(_ext_antenna_enable);
}

bool HalEsp32::getExtAntennaEnable()
{
    return _ext_antenna_enable;
}

void HalEsp32::startWifiAp()
{
    wifi_init();
}
