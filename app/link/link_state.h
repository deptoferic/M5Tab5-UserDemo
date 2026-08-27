/*
 * link_state.h — what the Tab5 knows about the satellite.
 *
 * THIS IS THE §6 SEAM. The link task writes it; rendering reads it and does
 * nothing else. Rendering must never touch the UART, never send a command and
 * never wait on a response. If a render function needs something that is not
 * here, add it here — do not reach for the transport.
 *
 * Deliberately platform-independent and deliberately NOT the wire format:
 *   - it does not include link_schema.h, so app/ still builds on desktop
 *     where shared/ is not wired in;
 *   - it is UI-facing state, not frames. Translation happens in the transport.
 *
 * Concurrency: single writer (the link task), many readers (render). Guarded by
 * a seqlock rather than a mutex — portable C, no OS primitives, and a reader can
 * never block the writer. Readers retry on a torn read.
 */
#ifndef LINK_STATE_H
#define LINK_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LINK_DOWN = 0,        /* nothing heard, or heard and rejected */
    LINK_HANDSHAKING,     /* hello sent, awaiting a valid response */
    LINK_UP,              /* schema agreed, frames flowing        */
    LINK_SCHEMA_MISMATCH, /* peer disagrees on the contract — fatal, not retried */
} link_status_t;

typedef struct {
    link_status_t status;

    /* peer identity */
    uint32_t peer_schema_ver;
    uint32_t boot_id;           /* satellite boot identity; a change means it restarted */
    uint32_t sat_uptime_ms;     /* as reported at the last hello */

    /* time, per D2: the satellite stamps monotonic ms since ITS boot and no
     * wall clock crosses the link. This is the mapping we derive locally. */
    int64_t  sat_boot_wall_ms;  /* our wall clock at the satellite's boot instant */
    uint32_t last_ts_ms;        /* most recent satellite monotonic stamp seen    */
    int64_t  last_wall_ms;      /* that stamp mapped through the mapping above   */

    /* counters — the evidence that the link is working, or how it is failing */
    uint32_t rx_frames;
    uint32_t rx_evt;
    uint32_t rx_rsp;
    uint32_t rx_parse_err;
    uint32_t rx_invalid;
    uint32_t rx_overflow;
    uint32_t resyncs;
    uint32_t tx_cmds;
    uint32_t rsp_timeouts;
    uint32_t reboots_seen;      /* boot_id changes observed */

    int64_t  last_frame_wall_ms; /* our wall clock when anything last arrived */
    char     last_line[160];     /* most recent complete line, for a debug readout */
} link_state_t;

/* Reader side. Safe from any task. Copies a coherent snapshot. */
void link_state_read(link_state_t *out);

/* Writer side — the link task only. Bracket mutations between these. */
link_state_t *link_state_begin_write(void);
void          link_state_end_write(void);

#ifdef __cplusplus
}
#endif
#endif /* LINK_STATE_H */
