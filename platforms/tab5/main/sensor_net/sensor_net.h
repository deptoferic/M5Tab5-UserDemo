/*
 * sensor_net.h — the Tab5 as the sensor network's AP and receiver.
 *
 * CLAUDE.md §2.5. The Tab5 raises its own softAP, sensor nodes JOIN IT as
 * stations, and readings arrive as HTTP POSTs. No router, and the satellite is
 * not in this path.
 *
 * WHAT THIS IS NOT: it is not a web UI and must not become one. The endpoints
 * here exist to RECEIVE readings and to make the table inspectable while there
 * is nothing to render. The face is LVGL on the panel, reading node_table.h
 * through the §6 seam. If you are about to serve a page with a control on it,
 * you are building the wrong thing in the wrong place.
 *
 * THE TAB5 NOW CARRIES THE SENSOR PATH, AND THAT IS A REAL COST — see §2.5.
 * Under §2.2 the satellite is the system of record so that monitoring survives
 * the display dying. With the Tab5 as the AP, a dead Tab5 takes the network
 * with it and the nodes have nobody to talk to. That trade is the operator's,
 * it is recorded rather than hidden, and it is the one thing to reopen first if
 * this architecture disappoints.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the AP if it is not already up, then serve the sensor endpoints.
 * Safe to call more than once. Returns false if the radio never came up. */
bool sensor_net_start(void);

/* Register the sensor endpoints onto an EXISTING server, for the case where
 * something else (M5's HAL) already owns port 80. */
void sensor_net_attach(httpd_handle_t server);

/* Console: `nodes`, and radio state reporting. */
void sensor_net_register_console(void);

#ifdef __cplusplus
}
#endif
