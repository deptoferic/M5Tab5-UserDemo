#include <string.h>
#include "link/link_state.h"

/* Seqlock. Odd sequence == a write is in progress. */
static volatile uint32_t s_seq;
static link_state_t      s_state;

#if defined(__GNUC__)
#  define LS_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
#  define LS_BARRIER() do {} while (0)
#endif

link_state_t *link_state_begin_write(void)
{
    s_seq++;              /* -> odd */
    LS_BARRIER();
    return &s_state;
}

void link_state_end_write(void)
{
    LS_BARRIER();
    s_seq++;              /* -> even */
}

void link_state_read(link_state_t *out)
{
    uint32_t before, after;
    do {
        before = s_seq;
        LS_BARRIER();
        memcpy(out, &s_state, sizeof(*out));
        LS_BARRIER();
        after = s_seq;
    } while ((before & 1u) || before != after);   /* retry on a torn read */
}
