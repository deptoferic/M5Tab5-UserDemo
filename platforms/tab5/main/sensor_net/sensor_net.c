#include "sensor_net.h"
#include "sensors/node_table.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_console.h"
#include "cJSON.h"

static const char *TAG = "sensornet";

#define REPORT_INTERVAL_MS 5000     /* what nodes are told to use; staleness derives from it */

static httpd_handle_t s_server;
static bool           s_attached;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ------------------------------------------------------------------ */
/* POST /reading — the whole sensor path, hub side                      */
/* ------------------------------------------------------------------ */

static esp_err_t reading_post(httpd_req_t *req)
{
    char buf[320];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        got += r;
    }
    buf[got] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        /* Reject loudly rather than storing a guess. A malformed reading that
         * lands in the table is worse than one that never arrives: the second
         * shows as a gap, the first shows as truth. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }

    const cJSON *jid   = cJSON_GetObjectItem(root, "node");
    const cJSON *jtemp = cJSON_GetObjectItem(root, "temp_c");
    const cJSON *jseq  = cJSON_GetObjectItem(root, "seq");
    const cJSON *jup   = cJSON_GetObjectItem(root, "uptime_ms");
    const cJSON *jok   = cJSON_GetObjectItem(root, "ok");
    const cJSON *jwhy  = cJSON_GetObjectItem(root, "why");

    if (!cJSON_IsString(jid) || !cJSON_IsNumber(jtemp)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fields");
        return ESP_FAIL;
    }

    bool ok = node_table_report(jid->valuestring,
                                (float)jtemp->valuedouble,
                                cJSON_IsBool(jok) ? cJSON_IsTrue(jok) : true,
                                cJSON_IsNumber(jseq) ? (uint32_t)jseq->valuedouble : 0,
                                cJSON_IsNumber(jup)  ? (uint32_t)jup->valuedouble  : 0,
                                now_ms(),
                                cJSON_IsString(jwhy) && strcmp(jwhy->valuestring, "threshold") == 0);
    cJSON_Delete(root);

    if (!ok) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "table full"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}\n", HTTPD_RESP_USE_STRLEN);
}

/* GET /nodes — the table as JSON. Diagnostics while nothing renders. */
static esp_err_t nodes_get(httpd_req_t *req)
{
    node_table_t t;
    node_table_snapshot(&t);
    int64_t n = now_ms();

    char buf[1024];
    int w = snprintf(buf, sizeof(buf),
                     "{\"total_rx\":%u,\"total_reject\":%u,\"node_count\":%u,\"nodes\":[",
                     (unsigned)t.total_rx, (unsigned)t.total_reject, (unsigned)t.node_count);
    bool first = true;
    for (int i = 0; i < NODE_MAX && w < (int)sizeof(buf) - 200; i++) {
        const node_entry_t *e = &t.nodes[i];
        if (!e->used) { continue; }
        w += snprintf(buf + w, sizeof(buf) - w,
                      "%s{\"id\":\"%s\",\"temp_c\":%.2f,\"valid\":%s,\"seq\":%u,"
                      "\"rx\":%u,\"gaps\":%u,\"timer\":%u,\"event\":%u,\"age_ms\":%lld,\"stale\":%s}",
                      first ? "" : ",", e->id, e->temp_c, e->temp_valid ? "true" : "false",
                      (unsigned)e->node_seq, (unsigned)e->rx_count, (unsigned)e->gap_count,
                      (unsigned)e->timer_count, (unsigned)e->event_count,
                      (long long)(n - e->last_heard_ms),
                      node_is_stale(e, n, REPORT_INTERVAL_MS) ? "true" : "false");
        first = false;
    }
    w += snprintf(buf + w, sizeof(buf) - w, "]}\n");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, w);
}

void sensor_net_attach(httpd_handle_t server)
{
    if (!server || s_attached) { return; }
    static const httpd_uri_t r = { "/reading", HTTP_POST, reading_post, NULL };
    static const httpd_uri_t n = { "/nodes",   HTTP_GET,  nodes_get,   NULL };
    httpd_register_uri_handler(server, &r);
    httpd_register_uri_handler(server, &n);
    s_server   = server;
    s_attached = true;
    ESP_LOGI(TAG, "ATTACH|/reading and /nodes registered");
}

/* The AP bring-up lives in the HAL (hal_wifi.cpp) and is reached through these
 * shims. Deliberately NOT reimplemented here: the wifi_remote AP netif glue is
 * hand-rolled because esp_netif_create_default_wifi_ap() is declared but never
 * defined for that component, and a second copy would drift from the first. */
extern void           hal_wifi_start_ap_c(void);
extern httpd_handle_t hal_wifi_get_server_c(void);

bool sensor_net_start(void)
{
    wifi_mode_t m = WIFI_MODE_NULL;
    bool up = (esp_wifi_get_mode(&m) == ESP_OK) && (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA);
    if (!up) {
        hal_wifi_start_ap_c();
    }
    httpd_handle_t s = hal_wifi_get_server_c();
    if (s) { sensor_net_attach(s); }
    return (esp_wifi_get_mode(&m) == ESP_OK) && (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA);
}

/* ------------------------------------------------------------------ */
/* Radio state — reported, never assumed                                */
/* ------------------------------------------------------------------ */

static void print_radio(void)
{
    wifi_mode_t m = WIFI_MODE_NULL;
    esp_err_t e = esp_wifi_get_mode(&m);
    if (e != ESP_OK) {
        /* This is the honest answer when the C6 is not up: the host cannot even
         * ask. Distinguish it from "mode is NULL", which means the stack IS up
         * and simply idle. */
        printf("NET|wifi_get_mode FAILED: %s (radio not initialised)\n", esp_err_to_name(e));
        return;
    }
    const char *ms = (m == WIFI_MODE_AP) ? "AP" : (m == WIFI_MODE_STA) ? "STA"
                   : (m == WIFI_MODE_APSTA) ? "APSTA" : "NULL";
    printf("NET|mode=%s\n", ms);

    if (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA) {
        wifi_config_t c = {0};
        if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {
            printf("NET|ap ssid='%s' channel=%u max_conn=%u authmode=%d\n",
                   (char *)c.ap.ssid, (unsigned)c.ap.channel,
                   (unsigned)c.ap.max_connection, (int)c.ap.authmode);
        }
        wifi_sta_list_t sl = {0};
        if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) {
            printf("NET|clients=%d\n", sl.num);
            for (int i = 0; i < sl.num; i++) {
                printf("NET|client %02x:%02x:%02x:%02x:%02x:%02x rssi=%d\n",
                       sl.sta[i].mac[0], sl.sta[i].mac[1], sl.sta[i].mac[2],
                       sl.sta[i].mac[3], sl.sta[i].mac[4], sl.sta[i].mac[5],
                       sl.sta[i].rssi);
            }
        }
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t ip;
        if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK) {
            printf("NET|ap_ip=" IPSTR "\n", IP2STR(&ip.ip));
        }
    }
    printf("NET|httpd=%s endpoints=%s\n", s_server ? "up" : "down", s_attached ? "yes" : "no");
}

static int cmd_nodes(int argc, char **argv)
{
    (void)argc; (void)argv;
    node_table_t t;
    node_table_snapshot(&t);
    int64_t n = now_ms();
    printf("NODES|total_rx=%u reject=%u count=%u\n",
           (unsigned)t.total_rx, (unsigned)t.total_reject, (unsigned)t.node_count);
    for (int i = 0; i < NODE_MAX; i++) {
        const node_entry_t *e = &t.nodes[i];
        if (!e->used) { continue; }
        printf("NODES|%s temp_c=%.2f valid=%d seq=%u rx=%u gaps=%u timer=%u event=%u age_ms=%lld stale=%d\n",
               e->id, e->temp_c, e->temp_valid, (unsigned)e->node_seq,
               (unsigned)e->rx_count, (unsigned)e->gap_count,
               (unsigned)e->timer_count, (unsigned)e->event_count,
               (long long)(n - e->last_heard_ms),
               node_is_stale(e, n, REPORT_INTERVAL_MS));
    }
    return 0;
}

/* CONTROL EXPERIMENT. If the C6 can SEE networks as a station, then the radio,
 * the SDIO link and the antenna all work, and any AP failure is specific to AP
 * mode. Without this, "no beacon" and "dead radio" are indistinguishable — and
 * they call for completely different responses. */
static void do_scan(void)
{
    wifi_mode_t prev = WIFI_MODE_NULL;
    esp_wifi_get_mode(&prev);

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
        printf("NET|scan: cannot enter APSTA\n");
        return;
    }
    wifi_scan_config_t sc = {0};
    sc.show_hidden = true;
    printf("NET|scan starting (2.4 GHz)\n");
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    if (e != ESP_OK) {
        printf("NET|scan FAILED err=%s\n", esp_err_to_name(e));
        esp_wifi_set_mode(prev);
        return;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 15) { n = 15; }
    static wifi_ap_record_t recs[15];
    esp_wifi_scan_get_ap_records(&n, recs);
    printf("NET|scan found=%u\n", (unsigned)n);
    for (int i = 0; i < n; i++) {
        printf("NET|scan ssid='%s' ch=%u rssi=%d\n",
               (const char *)recs[i].ssid, (unsigned)recs[i].primary, (int)recs[i].rssi);
    }
    esp_wifi_set_mode(prev);
}

/* Antenna select. The C6-MINI-1U has no onboard antenna, so this switch decides
 * whether anything radiates at all. Never called at boot by the stock firmware —
 * only from the launcher's UI switch — so its state is whatever the PI4IO
 * expander powers up in. Exposed here to make that testable rather than assumed. */
static int cmd_ant(int argc, char **argv)
{
    if (argc >= 2) {
        bool ext = atoi(argv[1]) != 0;
        extern void bsp_set_ext_antenna_enable(bool en);
        bsp_set_ext_antenna_enable(ext);
        printf("NET|antenna=%s\n", ext ? "EXTERNAL" : "INTERNAL");
    } else {
        printf("NET|usage: ant <0=internal|1=external>\n");
    }
    return 0;
}

/* Re-apply the AP config in a given mode / channel. Some ESP-Hosted slave
 * builds behave differently in APSTA than in pure AP, and the channel is worth
 * varying because the C6 demonstrably works on 6 (it hears the router there). */
static void ap_reconfig(wifi_mode_t mode, int channel)
{
    esp_err_t e = esp_wifi_set_mode(mode);
    printf("NET|set_mode=%d -> %s\n", (int)mode, esp_err_to_name(e));

    wifi_config_t c = {0};
    esp_wifi_get_config(WIFI_IF_AP, &c);
    if (channel > 0) { c.ap.channel = (uint8_t)channel; }
    if (c.ap.ssid[0] == 0) {
        strcpy((char *)c.ap.ssid, "M5Tab5-UserDemo-WiFi");
        c.ap.ssid_len = strlen("M5Tab5-UserDemo-WiFi");
    }
    c.ap.max_connection = 10;
    c.ap.ssid_hidden    = 0;
    e = esp_wifi_set_config(WIFI_IF_AP, &c);
    printf("NET|set_config -> %s (ssid='%s' ch=%u hidden=%u)\n",
           esp_err_to_name(e), (char *)c.ap.ssid, (unsigned)c.ap.channel,
           (unsigned)c.ap.ssid_hidden);
    e = esp_wifi_start();
    printf("NET|start -> %s\n", esp_err_to_name(e));
}

static int cmd_net(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "scan") == 0) { do_scan(); return 0; }
    if (argc >= 2 && strcmp(argv[1], "apsta") == 0) {
        ap_reconfig(WIFI_MODE_APSTA, argc >= 3 ? atoi(argv[2]) : 0);
        print_radio(); return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "ap") == 0) {
        ap_reconfig(WIFI_MODE_AP, argc >= 3 ? atoi(argv[2]) : 0);
        print_radio(); return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "start") == 0) {
        printf("NET|starting AP\n");
        sensor_net_start();
    }
    print_radio();
    return 0;
}

void sensor_net_register_console(void)
{
    node_table_init();
    const esp_console_cmd_t nodes_cmd = {
        .command = "nodes", .help = "Sensor node table, read through the §6 seam",
        .hint = NULL, .func = &cmd_nodes,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nodes_cmd));
    const esp_console_cmd_t ant_cmd = {
        .command = "ant", .help = "Antenna select: ant <0=internal|1=external>",
        .hint = NULL, .func = &cmd_ant,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ant_cmd));
    const esp_console_cmd_t net_cmd = {
        .command = "net", .help = "Sensor-network radio state. `net start` raises the AP, `net scan` lists visible APs",
        .hint = NULL, .func = &cmd_net,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&net_cmd));
}
