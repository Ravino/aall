/* bt/bt_ops_fake.h -- deterministic in-memory struct m4_bt_ops
 * implementation for tests (mirrors core/graph_ops_fake.h's role for
 * struct graph_ops) -- no real D-Bus/system bus connection. Used by
 * tests/test_repair_a2dp.c and tests/test_callfsm_graph_adapter.c's
 * repair-a2dp scenarios.
 */
#ifndef MOMENTUM4_BT_BT_OPS_FAKE_H
#define MOMENTUM4_BT_BT_OPS_FAKE_H

#include "bt_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int connect_profile_calls;
	char last_mac_address[32];
	char last_bt_adapter[16];
	char last_uuid[48];
	int last_timeout_ms;

	/* What the NEXT (and every subsequent, until changed again)
	 * connect_profile() call returns -- 0 (success) by default. */
	int result;
	char result_message[192];

	/* Internal: the vtable m4_bt_ops_fake() hands out, embedded here
	 * (not a shared/static one, unlike core/graph_ops_fake.c's trick --
	 * m4_bt_ops already has a per-instance `ctx` field, so embedding is
	 * simpler and lets multiple fakes be used independently, even at the
	 * same time, in one test). Do not touch directly. */
	struct m4_bt_ops _ops;
} m4_bt_ops_fake_t;

/* result=0 (success), everything else zeroed. */
void m4_bt_ops_fake_init(m4_bt_ops_fake_t *fake);

/* Returns a `struct m4_bt_ops` bound to `fake` via its own `ctx` field.
 * `fake` must outlive the returned pointer's use. */
const struct m4_bt_ops *m4_bt_ops_fake(m4_bt_ops_fake_t *fake);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_BT_BT_OPS_FAKE_H */
