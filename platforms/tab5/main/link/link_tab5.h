/*
 * link_tab5.h — the Tab5's half of Schema A. Phase 1c.
 *
 * Owns UART1 on the Grove pins and writes everything it learns into
 * link_state (app/link/link_state.h). It renders nothing and knows nothing
 * about LVGL — that is the §6 hard rule, and this file is the side of the seam
 * that is allowed to talk to hardware.
 */
#ifndef LINK_TAB5_H
#define LINK_TAB5_H

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up UART1 and starts the link task. Safe to call once, from app_main. */
void link_tab5_start(void);

/* Force a fresh hello, re-deriving the monotonic->wall-clock anchor.
 *
 * The anchor is computed once at hello from OUR clock. If our clock then jumps
 * — an RTC being set, an NTP step — the anchor is stale and every mapped
 * timestamp is wrong by the size of the jump, silently. Call this after any
 * deliberate change to the system clock. */
void link_tab5_resync_clock(void);

#ifdef __cplusplus
}
#endif
#endif
