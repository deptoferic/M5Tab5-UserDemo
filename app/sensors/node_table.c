#include "node_table.h"
#include <string.h>

/* Seqlock. Odd sequence == a write is in progress. Mirrors link_state.c so the
 * two seams behave identically; a reader that knows one knows the other. */
static volatile uint32_t s_seq;
static node_table_t      s_tab;

static void write_begin(void) { s_seq++; __sync_synchronize(); }
static void write_end(void)   { __sync_synchronize(); s_seq++; }

void node_table_init(void)
{
    write_begin();
    memset(&s_tab, 0, sizeof(s_tab));
    write_end();
}

static node_entry_t *find_or_claim(const char *id)
{
    node_entry_t *free_slot = NULL;
    for (int i = 0; i < NODE_MAX; i++) {
        node_entry_t *n = &s_tab.nodes[i];
        if (n->used && strncmp(n->id, id, NODE_ID_LEN - 1) == 0) { return n; }
        if (!n->used && !free_slot) { free_slot = n; }
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        strncpy(free_slot->id, id, NODE_ID_LEN - 1);
        free_slot->used = true;
        s_tab.node_count++;
    }
    return free_slot;   /* NULL == table full; the caller counts that as a reject */
}

bool node_table_report(const char *id, float temp_c, bool temp_valid,
                       uint32_t seq, uint32_t node_uptime_ms, int64_t now_ms,
                       bool by_threshold)
{
    if (!id || !id[0]) { s_tab.total_reject++; return false; }

    write_begin();
    node_entry_t *n = find_or_claim(id);
    if (!n) {
        s_tab.total_reject++;
        write_end();
        return false;
    }

    /* Count what did NOT arrive. A gap is the only evidence of loss we can have,
     * because absolute state means a missing reading leaves no other trace —
     * which is exactly the property that makes absolute state safe (§2.4). */
    if (n->rx_count > 0 && seq > n->node_seq + 1) {
        n->gap_count += (seq - n->node_seq - 1);
    }
    /* A node reboot restarts its seq, so seq goes BACKWARDS rather than jumping
     * forward. The condition above already ignores that, which is why there is
     * no reboot branch here: a restart must not be counted as radio loss, and
     * the ordering does that for free. Same reasoning as boot_id on the link. */

    n->temp_c         = temp_c;
    n->temp_valid     = temp_valid;
    n->node_seq       = seq;
    n->node_uptime_ms = node_uptime_ms;
    n->last_heard_ms  = now_ms;
    n->rx_count++;
    if (by_threshold) { n->event_count++; } else { n->timer_count++; }
    s_tab.total_rx++;
    write_end();
    return true;
}

void node_table_snapshot(node_table_t *out)
{
    uint32_t before, after;
    do {
        before = s_seq;
        __sync_synchronize();
        memcpy(out, &s_tab, sizeof(*out));
        __sync_synchronize();
        after = s_seq;
    } while ((before & 1u) || before != after);
}

bool node_is_stale(const node_entry_t *n, int64_t now_ms, uint32_t interval_ms)
{
    if (!n->used || n->rx_count == 0) { return true; }
    return (now_ms - n->last_heard_ms) > (int64_t)(interval_ms * NODE_STALE_MISSES);
}
