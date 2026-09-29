/* tests/test_systemd_unit_search.c -- util/systemd_unit.c's pure, offline
 * m4_systemd_unit_dir_in_search_path() (migration fix:
 * `[paths] systemd-user-unit-dir` must resolve to a directory systemd
 * --user actually scans, not just a syntactically valid path).
 *
 * Every check below sets/clears the exact XDG_... / HOME env vars the
 * function reads (never inherits whatever the surrounding shell has --
 * see setenv()/unsetenv() calls in each test) so this test's result does
 * not depend on the machine or session it happens to run on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/systemd_unit.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                        \
	do {                                                                     \
		checks++;                                                        \
		if (!(cond)) {                                                   \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++;                                              \
		}                                                                \
	} while (0)

static void clear_env(void)
{
	unsetenv("HOME");
	unsetenv("XDG_CONFIG_HOME");
	unsetenv("XDG_CONFIG_DIRS");
	unsetenv("XDG_DATA_HOME");
	unsetenv("XDG_DATA_DIRS");
}

/* System-wide prefixes: /usr and /usr/local's own "lib/systemd/user" are
 * ALWAYS in the search path, independent of any XDG_* variable -- systemd
 * hardcodes both. */
static void test_system_prefixes_always_searched(void)
{
	clear_env();
	CHECK(m4_systemd_unit_dir_in_search_path("/usr/lib/systemd/user"), "/usr/lib/systemd/user always searched");
	CHECK(m4_systemd_unit_dir_in_search_path("/usr/local/lib/systemd/user"),
	      "/usr/local/lib/systemd/user always searched");
	/* .../share/... is ALSO searched under the /usr* default XDG_DATA_DIRS --
	 * this is exactly why a non-/usr* prefix should use "share", not "lib". */
	CHECK(m4_systemd_unit_dir_in_search_path("/usr/share/systemd/user"), "/usr/share/systemd/user (default XDG_DATA_DIRS)");
	CHECK(m4_systemd_unit_dir_in_search_path("/usr/local/share/systemd/user"),
	      "/usr/local/share/systemd/user (default XDG_DATA_DIRS)");
}

/* The exact bug this fix closes: PREFIX=$HOME/.local's OLD default
 * ("$HOME/.local/lib/systemd/user") is NOT searched; the NEW default
 * ("$HOME/.local/share/systemd/user", == default $XDG_DATA_HOME) IS. */
static void test_home_local_prefix(void)
{
	clear_env();
	setenv("HOME", "/home/u", 1);
	CHECK(!m4_systemd_unit_dir_in_search_path("/home/u/.local/lib/systemd/user"),
	      "~/.local/lib/systemd/user (old, broken default) is NOT searched");
	CHECK(m4_systemd_unit_dir_in_search_path("/home/u/.local/share/systemd/user"),
	      "~/.local/share/systemd/user (new default, == $XDG_DATA_HOME) IS searched");
}

/* Explicit XDG_DATA_HOME overrides the ~/.local/share default entirely. */
static void test_xdg_data_home_override(void)
{
	clear_env();
	setenv("HOME", "/home/u", 1);
	setenv("XDG_DATA_HOME", "/custom/data", 1);
	CHECK(m4_systemd_unit_dir_in_search_path("/custom/data/systemd/user"), "XDG_DATA_HOME override searched");
	CHECK(!m4_systemd_unit_dir_in_search_path("/home/u/.local/share/systemd/user"),
	      "~/.local/share/systemd/user no longer searched once XDG_DATA_HOME is set");
}

/* XDG_DATA_DIRS/XDG_CONFIG_DIRS are ':'-separated lists -- every element
 * must be checked, not just the first. */
static void test_colon_separated_lists(void)
{
	clear_env();
	setenv("XDG_DATA_DIRS", "/opt/a/share:/opt/b/share", 1);
	CHECK(m4_systemd_unit_dir_in_search_path("/opt/a/share/systemd/user"), "first XDG_DATA_DIRS entry");
	CHECK(m4_systemd_unit_dir_in_search_path("/opt/b/share/systemd/user"), "second XDG_DATA_DIRS entry");
	CHECK(!m4_systemd_unit_dir_in_search_path("/opt/c/share/systemd/user"), "unrelated dir not searched");
}

/* Unrelated/arbitrary paths (the class of bug this exists to catch) --
 * never matched, no matter what env is set. */
static void test_arbitrary_path_not_searched(void)
{
	clear_env();
	setenv("HOME", "/home/u", 1);
	CHECK(!m4_systemd_unit_dir_in_search_path("/opt/momentum4/systemd/user"), "arbitrary custom prefix");
	CHECK(!m4_systemd_unit_dir_in_search_path(""), "empty string");
	CHECK(!m4_systemd_unit_dir_in_search_path(NULL), "NULL");
}

int main(void)
{
	test_system_prefixes_always_searched();
	test_home_local_prefix();
	test_xdg_data_home_override();
	test_colon_separated_lists();
	test_arbitrary_path_not_searched();

	printf("%d checks run\n", checks);
	if (failures > 0) {
		printf("FAILED (%d/%d)\n", failures, checks);
		return 1;
	}
	printf("OK\n");
	return 0;
}
