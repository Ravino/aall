/* tests/test_wp_env.c -- pure-logic unit tests for cli/wp_env.c's
 * m4_wp_env_decide(). No file I/O, no PipeWire connection
 * -- the file-I/O wrapper (m4_cli_wp_env_run()) is exercised separately by
 * an isolated live check, not
 * here, same split as graph_host_build vs graph_host_run.
 */
#include <stdio.h>
#include <string.h>

#include "cli/wp_env.h"

static int g_failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			g_failures++; \
		} \
	} while (0)

static struct m4_build_info make_bi(const char *pw_version)
{
	struct m4_build_info bi;

	memset(&bi, 0, sizeof(bi));
	if (pw_version != NULL)
		snprintf(bi.pipewire_version, sizeof(bi.pipewire_version), "%s", pw_version);
	bi.patches_count = 14;
	bi.m4_src_files_count = 20;
	return bi;
}

static void test_enable_when_everything_matches(void)
{
	struct m4_build_info bi = make_bi("1.4.2");

	CHECK(m4_wp_env_decide(&bi, true, "1.4.2", true, true) == M4_WP_ENV_ENABLE,
	      "exact match + so present + system dir present must enable");
}

static void test_disable_no_build_info(void)
{
	struct m4_build_info bi = make_bi("1.4.2");

	CHECK(m4_wp_env_decide(&bi, false, "1.4.2", true, true) == M4_WP_ENV_DISABLE_NO_BUILD_INFO,
	      "build_info_loaded=false must disable regardless of bi contents");
	CHECK(m4_wp_env_decide(NULL, true, "1.4.2", true, true) == M4_WP_ENV_DISABLE_NO_BUILD_INFO,
	      "bi==NULL must disable even if build_info_loaded were somehow true");
}

static void test_disable_version_mismatch(void)
{
	struct m4_build_info bi = make_bi("1.4.2");

	CHECK(m4_wp_env_decide(&bi, true, "1.4.3", true, true) == M4_WP_ENV_DISABLE_VERSION_MISMATCH,
	      "different PipeWire version must disable");
	CHECK(m4_wp_env_decide(&bi, true, "1.4", true, true) == M4_WP_ENV_DISABLE_VERSION_MISMATCH,
	      "prefix match is NOT enough -- must be byte-for-byte equal");
	CHECK(m4_wp_env_decide(&bi, true, NULL, true, true) == M4_WP_ENV_DISABLE_VERSION_MISMATCH,
	      "NULL installed_pw_version must disable, not crash");
	CHECK(m4_wp_env_decide(&bi, true, "", true, true) == M4_WP_ENV_DISABLE_VERSION_MISMATCH,
	      "empty installed_pw_version must disable");
	{
		struct m4_build_info empty_bi = make_bi(NULL);
		CHECK(m4_wp_env_decide(&empty_bi, true, "1.4.2", true, true) == M4_WP_ENV_DISABLE_VERSION_MISMATCH,
		      "BUILD-INFO with an empty pipewire_version field must disable, never match anything");
		CHECK(m4_wp_env_decide(&empty_bi, true, "", true, true) == M4_WP_ENV_DISABLE_VERSION_MISMATCH,
		      "two empty strings must NOT be treated as a match");
	}
}

static void test_disable_so_missing(void)
{
	struct m4_build_info bi = make_bi("1.4.2");

	CHECK(m4_wp_env_decide(&bi, true, "1.4.2", false, true) == M4_WP_ENV_DISABLE_SO_MISSING,
	      "missing .so must disable even with a matching version");
}

static void test_disable_no_system_dir_wins_over_nothing_else(void)
{
	struct m4_build_info bi = make_bi("1.4.2");

	CHECK(m4_wp_env_decide(&bi, true, "1.4.2", true, false) == M4_WP_ENV_DISABLE_NO_SYSTEM_DIR,
	      "missing spa-system-dir must disable even when everything else matches "
	      "(this is the one that would break ALL audio, not just bluez5, if skipped)");
}

static void test_decision_str_never_null(void)
{
	enum m4_wp_env_decision all[] = {
		M4_WP_ENV_ENABLE,
		M4_WP_ENV_DISABLE_NO_BUILD_INFO,
		M4_WP_ENV_DISABLE_VERSION_MISMATCH,
		M4_WP_ENV_DISABLE_SO_MISSING,
		M4_WP_ENV_DISABLE_NO_SYSTEM_DIR,
	};
	size_t i;

	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		CHECK(m4_wp_env_decision_str(all[i]) != NULL, "decision_str must never return NULL");
}

int main(void)
{
	test_enable_when_everything_matches();
	test_disable_no_build_info();
	test_disable_version_mismatch();
	test_disable_so_missing();
	test_disable_no_system_dir_wins_over_nothing_else();
	test_decision_str_never_null();

	if (g_failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("test_wp_env: all checks passed\n");
	return 0;
}
