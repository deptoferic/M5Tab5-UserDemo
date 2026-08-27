/*
 * link_probe.c — proves shared/link_schema.h is reachable from the Tab5 build.
 *
 * Exists so that "the component is registered" and "the header actually
 * compiles into this binary" are separate, separately-verified claims. A
 * registered component whose include directory reaches no compile command is
 * not proof of anything — see the Tab5 Sensor Hub CLAUDE.md §7.4 on asserting
 * against artifacts rather than a green configure.
 *
 * The static assertions below fail the BUILD if the contract drifts, so this
 * keeps earning its place after 1c lands.
 */
#include "link_schema.h"

_Static_assert(LINK_SCHEMA_VER == 1,        "Schema A version drifted from the satellite's");
_Static_assert(LINK_BAUD == 115200,         "link baud drifted");
_Static_assert(LINK_MAX_LINE == 2048,       "max line drifted");
_Static_assert(LINK_HEARTBEAT_MS == 2000,   "heartbeat interval drifted");
_Static_assert(LINK_SEQ_NONE == 0u,         "reserved seq drifted");

/* Referenced so the translation unit is not optimised away entirely. */
const int link_probe_schema_ver = LINK_SCHEMA_VER;
