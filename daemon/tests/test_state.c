/* Unit tests for core/state.c: call-state file format,
 * safe-latch detection, daemon mode
 * persistence. Uses a throwaway mkdtemp() directory for every test --
 * never touches a real $XDG_RUNTIME_DIR.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/state.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

static void make_tmpdir(char *out, size_t out_len)
{
	snprintf(out, out_len, "/tmp/m4d-test-state.XXXXXX");
	CHECK(mkdtemp(out) != NULL, "mkdtemp");
}

static void test_write_read_remove_roundtrip(void)
{
	char dir[128];
	char profile[M4_STATE_PROFILE_LEN], reason[M4_STATE_REASON_LEN];

	make_tmpdir(dir, sizeof(dir));

	CHECK(!m4_state_present(dir), "no state initially");
	CHECK(m4_state_write(dir, "headset-head-unit", "manual") == 0, "write ok");
	CHECK(m4_state_present(dir), "state present after write");
	CHECK(m4_state_read(dir, profile, sizeof(profile), reason, sizeof(reason)) == 0, "read ok");
	CHECK(strcmp(profile, "headset-head-unit") == 0, "profile roundtrip");
	CHECK(strcmp(reason, "manual") == 0, "reason roundtrip");

	m4_state_remove(dir);
	CHECK(!m4_state_present(dir), "state gone after remove");
	m4_state_remove(dir); /* idempotent, must not crash/error */
}

static void test_write_default_reason(void)
{
	char dir[128];
	char profile[M4_STATE_PROFILE_LEN], reason[M4_STATE_REASON_LEN];

	make_tmpdir(dir, sizeof(dir));
	CHECK(m4_state_write(dir, "a2dp-sink", NULL) == 0, "write with NULL reason ok");
	CHECK(m4_state_read(dir, profile, sizeof(profile), reason, sizeof(reason)) == 0, "read ok");
	CHECK(strcmp(reason, "manual") == 0, "NULL reason defaults to manual");
}

/* Exact format compatibility: PROFILE=/SAVED_AT=/REASON=, one KEY=value
 * per line. A file written by any tool that follows this format (or a
 * legacy file missing REASON=) must be readable by C, and vice
 * versa -- this test writes the file by hand rather than
 * via m4_state_write(), to catch a format drift in either direction. */
static void test_bash_format_compat(void)
{
	char dir[128], path[256];
	char profile[M4_STATE_PROFILE_LEN], reason[M4_STATE_REASON_LEN];
	FILE *f;

	make_tmpdir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%s/call-state", dir);
	f = fopen(path, "w");
	CHECK(f != NULL, "fopen for hand-written call-state");
	fprintf(f, "PROFILE=headset-head-unit-cvsd\nSAVED_AT=1234567890\n");
	fclose(f); /* no REASON= at all -- legacy file, like call_state_reason()'s docstring describes */

	CHECK(m4_state_present(dir), "present");
	CHECK(m4_state_read(dir, profile, sizeof(profile), reason, sizeof(reason)) == 0,
	      "read ok despite missing REASON=");
	CHECK(strcmp(profile, "headset-head-unit-cvsd") == 0, "profile read from hand-written file");
	CHECK(reason[0] == '\0', "reason left empty (caller, e.g. m4_callfsm_reason(), applies the "
				  "'manual' backcompat default, not m4_state_read() itself)");
}

static void test_safe_latch(void)
{
	char dir[128], path[256];

	make_tmpdir(dir, sizeof(dir));
	CHECK(!m4_state_safe_latched(dir), "no latch initially");

	FILE *f;
	snprintf(path, sizeof(path), "%s/safe-latch", dir);
	f = fopen(path, "w");
	CHECK(f != NULL, "create safe-latch");
	fclose(f);

	CHECK(m4_state_safe_latched(dir), "latch detected");
	CHECK(unlink(path) == 0, "remove safe-latch");
	CHECK(!m4_state_safe_latched(dir), "latch gone");
}

static void test_mode_default_and_roundtrip(void)
{
	char dir[128];

	make_tmpdir(dir, sizeof(dir));
	CHECK(m4_state_mode_read(dir) == M4_DAEMON_MODE_OBSERVE, "missing mode file -> observe");

	CHECK(m4_state_mode_write(dir, M4_DAEMON_MODE_MANUAL) == 0, "write manual");
	CHECK(m4_state_mode_read(dir) == M4_DAEMON_MODE_MANUAL, "read back manual");

	CHECK(m4_state_mode_write(dir, M4_DAEMON_MODE_AUTO) == 0, "write auto");
	CHECK(m4_state_mode_read(dir) == M4_DAEMON_MODE_AUTO, "read back auto");
}

static void test_mode_parse(void)
{
	m4_daemon_mode_t m;

	CHECK(m4_daemon_mode_parse("observe", &m) == 0 && m == M4_DAEMON_MODE_OBSERVE, "observe");
	CHECK(m4_daemon_mode_parse("OBSERVE", &m) == 0 && m == M4_DAEMON_MODE_OBSERVE, "case-insensitive");
	CHECK(m4_daemon_mode_parse("manual", &m) == 0 && m == M4_DAEMON_MODE_MANUAL, "manual");
	CHECK(m4_daemon_mode_parse("off", &m) == 0 && m == M4_DAEMON_MODE_MANUAL, "off alias -> manual");
	CHECK(m4_daemon_mode_parse("auto", &m) == 0 && m == M4_DAEMON_MODE_AUTO, "auto");
	CHECK(m4_daemon_mode_parse("on", &m) == 0 && m == M4_DAEMON_MODE_AUTO, "on alias -> auto");
	CHECK(m4_daemon_mode_parse("bogus", &m) < 0, "unknown rejected");
	CHECK(m4_daemon_mode_parse(NULL, &m) < 0, "NULL rejected");

	CHECK(strcmp(m4_daemon_mode_name(M4_DAEMON_MODE_OBSERVE), "observe") == 0, "name observe");
	CHECK(strcmp(m4_daemon_mode_name(M4_DAEMON_MODE_MANUAL), "manual") == 0, "name manual");
	CHECK(strcmp(m4_daemon_mode_name(M4_DAEMON_MODE_AUTO), "auto") == 0, "name auto");
}

static void test_missing_runtime_dir_is_not_a_crash(void)
{
	char profile[M4_STATE_PROFILE_LEN], reason[M4_STATE_REASON_LEN];

	CHECK(!m4_state_present("/nonexistent/m4d-test/path"), "present() false for missing dir");
	CHECK(m4_state_read("/nonexistent/m4d-test/path", profile, sizeof(profile), reason,
			     sizeof(reason)) < 0,
	      "read() fails cleanly for missing dir");
	CHECK(!m4_state_safe_latched("/nonexistent/m4d-test/path"), "safe_latched() false for missing dir");
	CHECK(m4_state_mode_read("/nonexistent/m4d-test/path") == M4_DAEMON_MODE_OBSERVE,
	      "mode_read() defaults to observe for missing dir");
	m4_state_remove("/nonexistent/m4d-test/path"); /* must not crash */
}

int main(void)
{
	test_write_read_remove_roundtrip();
	test_write_default_reason();
	test_bash_format_compat();
	test_safe_latch();
	test_mode_default_and_roundtrip();
	test_mode_parse();
	test_missing_runtime_dir_is_not_a_crash();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("state: all checks passed\n");
	return 0;
}
