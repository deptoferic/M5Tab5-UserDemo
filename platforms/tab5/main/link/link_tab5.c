/*
 * link_tab5.c — Schema A, Tab5 side.
 *
 * Contract : shared/link_schema.h        (the SAME header the satellite compiles)
 * Framing  : docs/schema_a.md §3         (resync table implemented verbatim)
 * Pins     : CLAUDE.md §3.2, measured and proven — G53 TX, G54 RX
 * Seam     : app/link/link_state.h       (§6 — we write it, render reads it)
 */

#include <string.h>
#include <stdio.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "cJSON.h"

#include "link_schema.h"
#include "link/link_state.h"
#include "link_tab5.h"

static const char *TAG = "link";

/* Measured and proven 2026-08-26 — see CLAUDE.md §3.2. Both pins are freely
 * assignable, so if a rewire ever crosses them, swap these two numbers rather
 * than moving wires. */
/* UART2, NOT UART1.
 *
 * §3.2's "use UART1, never UART0" is the SATELLITE's constraint and does not
 * transfer: on the Tab5, UART0 is the console and UART1 is already taken by the
 * HAL's RS-485 driver (platforms/tab5/main/hal/components/hal_rs485.cpp,
 * UART_NUM_1). Taking UART1 here makes uart_driver_install fail with
 * "UART driver already installed", and with ESP_ERROR_CHECK around it that
 * aborts and the board bootloops. Cost an hour on 2026-08-26.
 *
 * The P4 has 5 HP UARTs, so 2 is free. The pins are set by the GPIO matrix, so
 * the UART number is independent of G53/G54. */
#define LK_UART      2
#define LK_TX_GPIO   53
#define LK_RX_GPIO   54
#define LK_RX_BUF    (LINK_MAX_LINE * 2)

static uint32_t s_seq_next = LINK_SEQ_FIRST;
static uint32_t s_hello_seq;
static int64_t  s_hello_sent_ms;
static volatile bool s_want_rehello;

static int64_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ------------------------------------------------------------------ */
/* TX                                                                   */
/* ------------------------------------------------------------------ */

static void send_line(cJSON *o)
{
    char *txt = cJSON_PrintUnformatted(o);
    if (!txt) { return; }
    size_t n = strlen(txt);
    if (n + 1 > LINK_MAX_LINE) {
        /* Never emit a line the peer is contractually required to discard. */
        ESP_LOGE(TAG, "TX|drop=oversize len=%u", (unsigned)n);
        cJSON_free(txt);
        return;
    }
    uart_write_bytes(LK_UART, txt, n);
    const char eol = LINK_EOL;
    uart_write_bytes(LK_UART, &eol, 1);
    cJSON_free(txt);
}

static uint32_t send_cmd(const char *op)
{
    uint32_t seq = s_seq_next++;
    if (seq == LINK_SEQ_NONE) { seq = s_seq_next++; }   /* 0 is reserved */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, LINK_F_TYPE, LINK_T_CMD);
    cJSON_AddNumberToObject(o, LINK_F_SEQ, (double)seq);
    cJSON_AddStringToObject(o, LINK_F_OP, op);
    send_line(o);
    cJSON_Delete(o);

    link_state_t *st = link_state_begin_write();
    st->tx_cmds++;
    link_state_end_write();
    return seq;
}

static void send_hello(void)
{
    s_hello_seq = send_cmd(LINK_OP_HELLO);
    s_hello_sent_ms = wall_ms();
    link_state_t *st = link_state_begin_write();
    if (st->status == LINK_DOWN) { st->status = LINK_HANDSHAKING; }
    link_state_end_write();
    ESP_LOGI(TAG, "TX|hello seq=%u ver=%d", (unsigned)s_hello_seq, LINK_SCHEMA_VER);
}

/* ------------------------------------------------------------------ */
/* Boot-id handling — D2                                                */
/*                                                                      */
/* boot rides on EVERY satellite frame, so a restart is detected on the  */
/* first frame after it rather than at the next handshake. When it       */
/* changes, the monotonic->wall mapping is meaningless and must be       */
/* rebuilt, so we drop to HANDSHAKING and re-hello.                      */
/* ------------------------------------------------------------------ */

static void note_boot_id(uint32_t boot)
{
    link_state_t *st = link_state_begin_write();
    bool changed = (st->boot_id != 0 && st->boot_id != boot);
    if (changed) {
        st->reboots_seen++;
        st->status = LINK_HANDSHAKING;
        st->sat_boot_wall_ms = 0;
    }
    st->boot_id = boot;
    link_state_end_write();
    if (changed) {
        ESP_LOGW(TAG, "PEER|reboot boot=%u — remapping clock, re-hello", (unsigned)boot);
        send_hello();
    }
}

/* ------------------------------------------------------------------ */
/* RX                                                                   */
/* ------------------------------------------------------------------ */

static void on_rsp(cJSON *root)
{
    const cJSON *jseq = cJSON_GetObjectItemCaseSensitive(root, LINK_F_SEQ);
    const cJSON *jok  = cJSON_GetObjectItemCaseSensitive(root, LINK_F_OK);
    if (!cJSON_IsNumber(jseq)) {
        link_state_t *st = link_state_begin_write(); st->rx_invalid++; link_state_end_write();
        return;
    }
    uint32_t seq = (uint32_t)jseq->valuedouble;

    if (seq == s_hello_seq && s_hello_seq != LINK_SEQ_NONE) {
        const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "ver");
        const cJSON *jup  = cJSON_GetObjectItemCaseSensitive(root, LINK_F_UPTIME_MS);
        uint32_t ver = cJSON_IsNumber(jver) ? (uint32_t)jver->valuedouble : 0;
        uint32_t up  = cJSON_IsNumber(jup)  ? (uint32_t)jup->valuedouble  : 0;

        link_state_t *st = link_state_begin_write();
        st->peer_schema_ver = ver;
        st->sat_uptime_ms   = up;
        if (ver != LINK_SCHEMA_VER) {
            /* D1: strict equality, and we do NOT proceed. Two binaries flashed
             * by hand either agree or someone made a mistake worth shouting
             * about. There is no negotiated range and no fallback. */
            st->status = LINK_SCHEMA_MISMATCH;
        } else {
            st->status = LINK_UP;
            /* D2: derive the mapping. The satellite told us how long it has
             * been up; our own clock (already synced from the RX8130 by the
             * HAL at boot) says when "now" is. Difference is its boot instant. */
            st->sat_boot_wall_ms = wall_ms() - (int64_t)up;
        }
        link_state_end_write();

        if (ver != LINK_SCHEMA_VER) {
            ESP_LOGE(TAG, "SCHEMA MISMATCH|ours=%d peer=%u — refusing to proceed",
                     LINK_SCHEMA_VER, (unsigned)ver);
            ESP_LOGE(TAG, "SCHEMA MISMATCH|reflash both boards from the same shared/link_schema.h");
        } else {
            ESP_LOGI(TAG, "LINK UP|ver=%u sat_uptime_ms=%u", (unsigned)ver, (unsigned)up);
        }
        s_hello_seq = LINK_SEQ_NONE;
        return;
    }

    link_state_t *st = link_state_begin_write();
    st->rx_rsp++;
    link_state_end_write();
    (void)jok;
}

static void on_evt(cJSON *root)
{
    const cJSON *jev = cJSON_GetObjectItemCaseSensitive(root, LINK_F_EV);
    const cJSON *jts = cJSON_GetObjectItemCaseSensitive(root, LINK_F_TS_MS);

    link_state_t *st = link_state_begin_write();
    st->rx_evt++;
    if (cJSON_IsNumber(jts)) {
        st->last_ts_ms = (uint32_t)jts->valuedouble;
        if (st->sat_boot_wall_ms) {
            st->last_wall_ms = st->sat_boot_wall_ms + (int64_t)st->last_ts_ms;
        }
    }
    link_state_end_write();

    /* D3: unknown events are IGNORED, not errors. That asymmetry against
     * unknown ops is what lets the satellite gain events without a version
     * bump. docs/schema_a.md §3. */
    (void)jev;
}

static void handle_line(const char *line, size_t len)
{
    {
        link_state_t *st = link_state_begin_write();
        st->rx_frames++;
        st->last_frame_wall_ms = wall_ms();
        size_t n = len < sizeof(st->last_line) - 1 ? len : sizeof(st->last_line) - 1;
        memcpy(st->last_line, line, n);
        st->last_line[n] = '\0';
        link_state_end_write();
    }

    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) {
        link_state_t *st = link_state_begin_write(); st->rx_parse_err++; link_state_end_write();
        char head[65]; size_t n = len < 64 ? len : 64;
        memcpy(head, line, n); head[n] = 0;
        ESP_LOGW(TAG, "RX|drop=parse head=%s", head);
        return;
    }

    const cJSON *jt   = cJSON_GetObjectItemCaseSensitive(root, LINK_F_TYPE);
    const cJSON *jbot = cJSON_GetObjectItemCaseSensitive(root, LINK_F_BOOT_ID);
    if (!cJSON_IsString(jt) || !jt->valuestring) {
        link_state_t *st = link_state_begin_write(); st->rx_invalid++; link_state_end_write();
        cJSON_Delete(root);
        return;
    }
    if (cJSON_IsNumber(jbot)) { note_boot_id((uint32_t)jbot->valuedouble); }

    if      (!strcmp(jt->valuestring, LINK_T_EVT)) { on_evt(root); }
    else if (!strcmp(jt->valuestring, LINK_T_RSP)) { on_rsp(root); }
    else {
        /* A cmd from the satellite is not part of Phase 1 in this direction. */
        link_state_t *st = link_state_begin_write(); st->rx_invalid++; link_state_end_write();
    }
    cJSON_Delete(root);
}

void link_tab5_resync_clock(void)
{
    /* Flag rather than calling send_hello() directly: the link task owns the
     * UART, and two tasks writing frames could interleave bytes mid-line. */
    s_want_rehello = true;
}

/* THE INVARIANT (docs/schema_a.md §3): always either accumulating a line or
 * discarding to the next newline. Never parsing a fragment. */
static void link_rx_task(void *arg)
{
    static char    line[LINK_MAX_LINE];
    static uint8_t chunk[256];
    size_t len = 0;
    bool   discarding = true;   /* first partial line after open is garbage */

    send_hello();

    while (1) {
        int n = uart_read_bytes(LK_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (b == (uint8_t)LINK_EOL) {
                if (discarding) { discarding = false; len = 0; continue; }
                line[len] = '\0';
                handle_line(line, len);
                len = 0;
                continue;
            }
            if (b == '\r' || b < 0x20) { continue; }
            if (discarding) { continue; }
            if (len + 1 >= LINK_MAX_LINE) {
                link_state_t *st = link_state_begin_write();
                st->rx_overflow++; st->resyncs++;
                link_state_end_write();
                discarding = true; len = 0;
                ESP_LOGW(TAG, "RX|drop=overflow limit=%d", LINK_MAX_LINE);
                continue;
            }
            line[len++] = (char)b;
        }

        if (s_want_rehello) {
            s_want_rehello = false;
            ESP_LOGI(TAG, "clock changed — re-deriving the wall-clock anchor");
            link_state_t *st = link_state_begin_write();
            st->sat_boot_wall_ms = 0;
            link_state_end_write();
            send_hello();
        }

        /* hello retry / timeout */
        if (s_hello_seq != LINK_SEQ_NONE &&
            (wall_ms() - s_hello_sent_ms) > LINK_RSP_TIMEOUT_MS) {
            link_state_t *st = link_state_begin_write();
            st->rsp_timeouts++;
            bool fatal = (st->status == LINK_SCHEMA_MISMATCH);
            link_state_end_write();
            if (!fatal) {
                ESP_LOGW(TAG, "hello timed out, retrying");
                send_hello();
            } else {
                s_hello_seq = LINK_SEQ_NONE;   /* do not retry a mismatch */
            }
        }
    }
}

/* Periodic proof that the seam works: reads through the PUBLIC reader API, the
 * same one rendering will use, so it exercises the seqlock read path rather
 * than peeking at the struct. Machine-parseable per §7.1.
 *
 * This is not rendering and does not violate §6 — it consumes state, it does
 * not draw, and nothing here reaches back into the transport. */
static void link_report_task(void *arg)
{
    static const char *NAMES[] = { "DOWN", "HANDSHAKING", "UP", "SCHEMA_MISMATCH" };
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        link_state_t s;
        link_state_read(&s);
        ESP_LOGI(TAG, "STATE|status=%s peer_ver=%u boot=%u frames=%u evt=%u rsp=%u "
                      "parse_err=%u invalid=%u overflow=%u resync=%u tx=%u timeouts=%u reboots=%u",
                 NAMES[s.status], (unsigned)s.peer_schema_ver, (unsigned)s.boot_id,
                 (unsigned)s.rx_frames, (unsigned)s.rx_evt, (unsigned)s.rx_rsp,
                 (unsigned)s.rx_parse_err, (unsigned)s.rx_invalid, (unsigned)s.rx_overflow,
                 (unsigned)s.resyncs, (unsigned)s.tx_cmds, (unsigned)s.rsp_timeouts,
                 (unsigned)s.reboots_seen);
        ESP_LOGI(TAG, "STATE|ts_ms=%u wall_ms=%lld last=%s",
                 (unsigned)s.last_ts_ms, (long long)s.last_wall_ms, s.last_line);
    }
}

void link_tab5_start(void)
{
    const uart_config_t cfg = {
        .baud_rate  = LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* Deliberately NOT ESP_ERROR_CHECK. The Tab5 is the face, not the brain
     * (§2.2) — it must not abort and bootloop because a link failed to come up.
     * A dead link is a state to display, not a reason to die. */
    esp_err_t err = uart_driver_install(LK_UART, LK_RX_BUF, LK_RX_BUF, 0, NULL, 0);
    if (err == ESP_OK) { err = uart_param_config(LK_UART, &cfg); }
    if (err == ESP_OK) {
        err = uart_set_pin(LK_UART, LK_TX_GPIO, LK_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "INIT FAILED|uart=%d err=%s — link stays down, UI continues",
                 LK_UART, esp_err_to_name(err));
        link_state_t *st = link_state_begin_write();
        st->status = LINK_DOWN;
        link_state_end_write();
        return;
    }

    xTaskCreate(link_rx_task, "link_rx", 5120, NULL, 9, NULL);
    xTaskCreate(link_report_task, "link_rep", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "INIT|schema=%d uart=%d tx=%d rx=%d baud=%d",
             LINK_SCHEMA_VER, LK_UART, LK_TX_GPIO, LK_RX_GPIO, LINK_BAUD);
}
