/* Unit test for the one pure helper struct graph_ops's pw/graph_pw.c
 * implementation exposes: m4_graph_find_profile_index() -- the same
 * name -> index lookup m4_graph_pw_set_profile() does internally before
 * ever calling pw_device_set_param() (mirrors module-protocol-pulse/
 * pulse-server.c's find_profile_index()). No live
 * PipeWire connection -- this only exercises struct m4_graph_state, a
 * plain data structure.
 *
 * The rest of pw/graph_pw.c's struct graph_ops implementation (the
 * persistent registry/subscribe/set_profile state machine) is NOT
 * exercised by `meson test` at all -- deliberately: never against a live
 * system during development, it was
 * verified by a manual, read-only, ad hoc run against the live PipeWire
 * instance and is not wired
 * into the automated suite, to avoid `meson test` depending on a live
 * PipeWire connection at all (this project's existing tests --
 * test_graph_props included -- never do that either).
 */
#include <stdio.h>
#include <string.h>

#include "core/graph_ops.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

static void set_profile(struct m4_graph_state *s, size_t i, uint32_t index, const char *name)
{
	s->profiles[i].index = index;
	snprintf(s->profiles[i].name, sizeof(s->profiles[i].name), "%s", name);
}

static void test_find_profile_index(void)
{
	struct m4_graph_state s;
	uint32_t idx;

	memset(&s, 0, sizeof(s));
	s.n_profiles = 4;
	set_profile(&s, 0, 0, "off");
	set_profile(&s, 1, 1, "a2dp-sink");
	set_profile(&s, 2, 2, "headset-head-unit");
	set_profile(&s, 3, 3, "headset-head-unit-cvsd");

	CHECK(m4_graph_find_profile_index(&s, "headset-head-unit", &idx) && idx == 2,
	      "finds headset-head-unit at its index");
	CHECK(m4_graph_find_profile_index(&s, "a2dp-sink", &idx) && idx == 1,
	      "finds a2dp-sink at its index");
	CHECK(!m4_graph_find_profile_index(&s, "a2dp-sink-aptx_hd", &idx),
	      "a profile family name that is not in the list is not found "
	      "(EnumProfile-only names like \"a2dp-sink-<codec>\" never appear "
	      "as the *active* profile on this device)");
	CHECK(!m4_graph_find_profile_index(&s, "", &idx), "empty name not found");
	CHECK(!m4_graph_find_profile_index(NULL, "off", &idx), "NULL state -> false, no crash");
	CHECK(!m4_graph_find_profile_index(&s, NULL, &idx), "NULL name -> false, no crash");
	CHECK(!m4_graph_find_profile_index(&s, "off", NULL), "NULL out_index -> false, no crash");

	s.n_profiles = 0;
	CHECK(!m4_graph_find_profile_index(&s, "off", &idx), "empty profiles[] -> never found");
}

int main(void)
{
	test_find_profile_index();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("graph_ops: all checks passed\n");
	return 0;
}
