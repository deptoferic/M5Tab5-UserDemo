/*
 * node_table.h — what the Tab5 knows about the sensor nodes.
 *
 * THIS IS A §6 SEAM, exactly like link_state.h and for the same reasons.
 * The receive path writes it; rendering reads a snapshot and does nothing else.
 * A render function must never call the HTTP layer, never touch a socket, and
 * never reach for a node by talking to it. If a panel needs something that is
 * not here, ADD IT HERE.
 *
 * Deliberately platform-independent and deliberately NOT the wire format: it
 * holds no JSON and no HTTP, so app/ still builds on desktop where there is no
 * radio. The desktop build can populate this table by hand and render states
 * that are awkward to produce on real hardware — a stale node, a threshold
 * breach — which is the whole reason §4.1 says UI work happens on desktop.
 *
 * Concurrency: single writer (the HTTP task), many readers. Seqlock, matching
 * link_state.h — portable C, no OS primitives, readers never block the writer.
 *
 * §2.4 STILL HOLDS ON THIS TRANSPORT. Nodes PUSH; the hub does not poll. Every
 * reading is ABSOLUTE STATE — a dropped POST costs one sample and nothing more.
 * `seq` exists so loss can be MEASURED, never so it can be reconstructed.
 *
 * SILENCE AND "UNCHANGED" LOOK IDENTICAL (§2.4 corollary), which is why every
 * entry carries last_heard_ms and why staleness is computed from it rather than
 * signalled by the node. A node that dies cannot send "I died".
 */
#ifndef NODE_TABLE_H
#define NODE_TABLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_MAX        8
#define NODE_ID_LEN     16

/* Stale after this many missed reports. Two, not one: a single late arrival is
 * normal on a shared radio, so flagging on one missed interval produces a table
 * that flickers and trains the operator to ignore it. */
#define NODE_STALE_MISSES  2.5f

typedef struct {
    char     id[NODE_ID_LEN];   /* node MAC, no separators — §8: identity is the silicon */
    bool     used;

    float    temp_c;            /* ABSOLUTE, as reported */
    bool     temp_valid;        /* the node said its own sensor read failed */
    uint32_t node_seq;          /* the node's counter — gaps here ARE loss  */
    uint32_t node_uptime_ms;    /* the node's own clock; a drop to ~0 means it rebooted */

    int64_t  last_heard_ms;     /* OUR clock at arrival — never the node's (§2.4) */
    uint32_t rx_count;          /* readings accepted from this node */
    uint32_t gap_count;         /* seq jumps > 1: readings that never arrived */
} node_entry_t;

typedef struct {
    node_entry_t nodes[NODE_MAX];
    uint32_t     total_rx;      /* readings accepted, all nodes */
    uint32_t     total_reject;  /* malformed or table-full */
    uint32_t     node_count;
} node_table_t;

void node_table_init(void);

/* Called by the receive path. Absolute state in, nothing accumulated.
 * Returns false only if the reading could not be stored at all. */
bool node_table_report(const char *id, float temp_c, bool temp_valid,
                       uint32_t seq, uint32_t node_uptime_ms, int64_t now_ms);

/* Reader side of the seam. Never blocks the writer; retries on a torn read. */
void node_table_snapshot(node_table_t *out);

/* Derived, not stored — staleness is a function of the clock, so storing it
 * would mean a node could look fresh simply because nothing recomputed it. */
bool node_is_stale(const node_entry_t *n, int64_t now_ms, uint32_t interval_ms);

#ifdef __cplusplus
}
#endif
#endif
