/*
 * console_tab5.cpp — a small console on the Tab5's USB-Serial-JTAG.
 *
 * Exists mainly for the RTC. The RX8130 is battery-backed and drifts, and the
 * factory demo can only set it through a touch panel — useless when the point
 * is to set it accurately from a host clock. §7.1: "a command surface is
 * testing"; passive log reading is only debugging.
 *
 * Shares the USB-Serial-JTAG channel with ESP_LOG output, the same arrangement
 * the satellite firmware uses.
 */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "esp_console.h"
#include "esp_log.h"
#include "argtable3/argtable3.h"

#include <hal/hal.h>
#include "link/link_state.h"
#include "link/link_tab5.h"

static const char *TAG = "con";

/* ---- rtc ------------------------------------------------------------ */

static void print_rtc(void)
{
    struct tm t = {};
    hal::Get()->getRtcTime(&t);
    time_t rtc_epoch = mktime(&t);

    struct timeval tv;
    gettimeofday(&tv, NULL);

    printf("RTC|rtc=%04d-%02d-%02d %02d:%02d:%02d epoch=%lld\n",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
           (long long)rtc_epoch);
    printf("RTC|sys_epoch=%lld delta_s=%lld\n",
           (long long)tv.tv_sec, (long long)(tv.tv_sec - rtc_epoch));
    printf("RTC|plausible=%s\n", (rtc_epoch > 1700000000LL) ? "yes" : "NO (pre-2023 — unset or flat cell)");
}

static int cmd_rtc(int argc, char **argv)
{
    print_rtc();
    return 0;
}

/* ---- rtcset <unix_epoch> -------------------------------------------- */
/*
 * Takes a UTC epoch rather than a formatted string: one integer, no timezone
 * ambiguity, and trivially supplied from a host with `date +%s`.
 */
static struct {
    struct arg_int *epoch;
    struct arg_end *end;
} rtcset_args;

static int cmd_rtcset(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&rtcset_args)) {
        arg_print_errors(stderr, rtcset_args.end, argv[0]);
        return 1;
    }
    time_t want = (time_t)rtcset_args.epoch->ival[0];

    struct tm t = {};
    gmtime_r(&want, &t);
    hal::Get()->setRtcTime(t);

    /* Push it into the system clock too, so link_state's wall-clock mapping
     * picks it up without a reboot. */
    struct timeval tv = { .tv_sec = want, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    /* The link's monotonic->wall anchor was derived from the OLD clock and is
     * now wrong by exactly the size of the jump. Re-derive it. */
    link_tab5_resync_clock();

    printf("RTCSET|wrote epoch=%lld\n", (long long)want);
    print_rtc();
    return 0;
}

/* ---- link ------------------------------------------------------------ */

static int cmd_link(int argc, char **argv)
{
    static const char *NAMES[] = { "DOWN", "HANDSHAKING", "UP", "SCHEMA_MISMATCH" };
    link_state_t s;
    link_state_read(&s);
    printf("LINK|status=%s peer_ver=%u boot=%u reboots=%u\n",
           NAMES[s.status], (unsigned)s.peer_schema_ver, (unsigned)s.boot_id,
           (unsigned)s.reboots_seen);
    printf("LINK|frames=%u evt=%u rsp=%u parse_err=%u invalid=%u overflow=%u resync=%u tx=%u timeouts=%u\n",
           (unsigned)s.rx_frames, (unsigned)s.rx_evt, (unsigned)s.rx_rsp,
           (unsigned)s.rx_parse_err, (unsigned)s.rx_invalid, (unsigned)s.rx_overflow,
           (unsigned)s.resyncs, (unsigned)s.tx_cmds, (unsigned)s.rsp_timeouts);
    /* The point of setting the RTC: this should be a real timestamp, not
     * correct arithmetic on a 2008 epoch. */
    time_t w = (time_t)(s.last_wall_ms / 1000);
    struct tm lt = {};
    gmtime_r(&w, &lt);
    printf("LINK|last_ts_ms=%u last_wall=%04d-%02d-%02d %02d:%02d:%02d UTC\n",
           (unsigned)s.last_ts_ms, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
           lt.tm_hour, lt.tm_min, lt.tm_sec);
    return 0;
}

/* --------------------------------------------------------------------- */

extern "C" void console_tab5_start(void)
{
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "tab5>";
    rc.task_stack_size = 8192;

    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    if (esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl) != ESP_OK) {
        ESP_LOGE(TAG, "console init failed — continuing without it");
        return;
    }
    esp_console_register_help_command();

    const esp_console_cmd_t c_rtc = { "rtc", "Read the RX8130 and the system clock", nullptr, &cmd_rtc, nullptr, nullptr };
    esp_console_cmd_register(&c_rtc);

    rtcset_args.epoch = arg_int1(nullptr, nullptr, "<unix_epoch>", "UTC seconds since 1970");
    rtcset_args.end   = arg_end(2);
    const esp_console_cmd_t c_set = { "rtcset", "Set the RX8130 from a UTC epoch", nullptr, &cmd_rtcset, &rtcset_args, nullptr };
    esp_console_cmd_register(&c_set);

    const esp_console_cmd_t c_link = { "link", "Link state, read through the §6 seam", nullptr, &cmd_link, nullptr, nullptr };
    esp_console_cmd_register(&c_link);

    esp_console_start_repl(repl);
    ESP_LOGI(TAG, "READY|console=usb_serial_jtag cmds=rtc,rtcset,link");
}
