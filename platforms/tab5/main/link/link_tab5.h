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

#ifdef __cplusplus
}
#endif
#endif
