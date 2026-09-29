/* cli/helper_exec.c -- see helper_exec.h. Does three
 * things, in order:
 *   1. Resolve/validate qemu/loader/capi (here: from `[helper]` via
 *      common/m4-conf, not a second hand-rolled config reader).
 *   2. Best-effort RT priority (here: sched_setscheduler() directly, not a
 *      chrt(1) subprocess) + nice + CPU affinity (new: the old wrapper
 *      never set affinity, `[helper] cpu-list` is a newer addition).
 *   3. exec (not fork+wait) qemu-hexagon, fd 0/1 untouched.
 */
#define _GNU_SOURCE
#include "helper_exec.h"

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "../../../common/m4-conf.h"
#include "../util/config.h"
#include "../util/log.h"
#include "../../../common/m4-i18n.h"

static bool conf_bool(const char *section, const char *key, bool fallback)
{
	return m4_conf_get_bool(m4_config(), section, key, fallback);
}

static long conf_int(const char *section, const char *key, long fallback)
{
	return m4_conf_get_int(m4_config(), section, key, fallback);
}

static const char *conf_str(const char *section, const char *key)
{
	return m4_conf_get_string(m4_config(), section, key);
}

/* "0,1" -> cpu_set_t with bits 0 and 1 set. Ignores anything it cannot
 * parse (best-effort, matches this whole file's "never a reason to fail
 * startup" philosophy for tuning knobs -- only the three PATH checks in
 * m4_cli_helper_exec_run() itself are fatal). */
static void apply_cpu_affinity(const char *cpu_list)
{
	cpu_set_t set;
	const char *p = cpu_list;
	bool any = false;

	if (cpu_list == NULL || cpu_list[0] == '\0')
		return;
	CPU_ZERO(&set);
	while (*p != '\0') {
		char *end = NULL;
		long cpu = strtol(p, &end, 10);

		if (end != p && cpu >= 0 && cpu < CPU_SETSIZE) {
			CPU_SET((int)cpu, &set);
			any = true;
		}
		p = end;
		while (*p == ',' || *p == ' ')
			p++;
	}
	if (any && sched_setaffinity(0, sizeof(set), &set) < 0)
		m4_log(M4_LOG_WARN, M4_T("helper-exec: sched_setaffinity(%s) failed: %s",
"helper-exec: sched_setaffinity(%s) не удался: %s"), cpu_list, strerror(errno));
}

/* "KEY=VAL,KEY2=VAL2" -> putenv() each (debug-only escape hatch, `[helper]
 * loader-env`'s own doc comment in m4-conf-keys.def). Silently skips a
 * malformed entry (no '=', or too long) rather than failing startup. */
static void apply_loader_env(const char *loader_env)
{
	const char *p = loader_env;

	if (loader_env == NULL || loader_env[0] == '\0')
		return;
	while (*p != '\0') {
		const char *comma = strchr(p, ',');
		size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);

		if (len > 0 && len < 256 && memchr(p, '=', len) != NULL) {
			char *entry = malloc(len + 1);

			if (entry != NULL) {
				memcpy(entry, p, len);
				entry[len] = '\0';
				if (putenv(entry) != 0) /* putenv() takes ownership on success only */
					free(entry);
			}
		}
		p = comma != NULL ? comma + 1 : p + len;
	}
}

static void setenv_int(const char *name, long value)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%ld", value);
	setenv(name, buf, 1);
}

int m4_cli_helper_exec_run(int argc, char **argv)
{
	const char *qemu_path;
	const char *loader_path;
	const char *capi_path;
	long rt_priority, nice_value, start_mute_pkts;
	const char *cpu_list;
	const char *loader_env;
	bool debug_log;

	(void)argc;
	(void)argv;

	qemu_path = conf_str("helper", "qemu");
	loader_path = conf_str("helper", "loader");
	capi_path = conf_str("helper", "capi");
	rt_priority = conf_int("helper", "rt-priority", 10);
	nice_value = conf_int("helper", "nice", -5);
	cpu_list = conf_str("helper", "cpu-list");
	start_mute_pkts = conf_int("helper", "start-mute-pkts", 0);
	loader_env = conf_str("helper", "loader-env");
	debug_log = conf_bool("helper", "debug-log", false);

	if (qemu_path == NULL || qemu_path[0] == '\0' || access(qemu_path, X_OK) != 0) {
		m4_log(M4_LOG_ERR, M4_T("helper-exec: qemu is not executable: %s (%s)",
"helper-exec: qemu не исполняемый: %s (%s)"),
		       qemu_path != NULL ? qemu_path : M4_T("(not set)",
"(не задан)"), strerror(errno));
		fprintf(stderr, "momentum4 helper-exec: qemu binary not executable: %s\n",
			qemu_path != NULL ? qemu_path : "(unset)");
		return 127;
	}
	if (loader_path == NULL || loader_path[0] == '\0' || access(loader_path, R_OK) != 0) {
		m4_log(M4_LOG_ERR, M4_T("helper-exec: loader is not readable: %s (%s)",
"helper-exec: loader не читается: %s (%s)"),
		       loader_path != NULL ? loader_path : M4_T("(not set)",
"(не задан)"), strerror(errno));
		fprintf(stderr, "momentum4 helper-exec: loader ELF not readable: %s\n",
			loader_path != NULL ? loader_path : "(unset)");
		return 127;
	}
	if (capi_path == NULL || capi_path[0] == '\0' || access(capi_path, R_OK) != 0) {
		m4_log(M4_LOG_ERR, M4_T("helper-exec: CAPI is not readable: %s (%s)",
"helper-exec: CAPI не читается: %s (%s)"),
		       capi_path != NULL ? capi_path : M4_T("(not set)",
"(не задан)"), strerror(errno));
		fprintf(stderr,
			M4_T("momentum4 helper-exec: CAPI .so not readable: %s (see `momentum4 setup --capi`)\n",
"momentum4 helper-exec: CAPI .so not readable: %s (см. `momentum4 setup --capi`)\n"),
			capi_path != NULL ? capi_path : "(unset)");
		return 127;
	}

	if (debug_log)
		m4_log(M4_LOG_INFO, "helper-exec: qemu=%s loader=%s capi=%s rt=%ld nice=%ld cpu=%s",
		       qemu_path, loader_path, capi_path, rt_priority, nice_value,
		       cpu_list != NULL && cpu_list[0] != '\0' ? cpu_list : M4_T("(any)",
"(любой)"));

	/* Best-effort RT/nice/affinity -- EPERM (no matching RLIMIT_RTPRIO,
	 * e.g. testing this outside the unit's LimitRTPRIO=) is silently
	 * tolerated, same "never a reason to fail startup" philosophy the old
	 * bash wrapper's own chrt(1) call already documented. */
	if (rt_priority > 0) {
		struct sched_param sp = {0};

		sp.sched_priority = (int)rt_priority;
		if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0 && errno != EPERM)
			m4_log(M4_LOG_WARN, M4_T("helper-exec: sched_setscheduler(SCHED_FIFO, %ld) failed: %s",
"helper-exec: sched_setscheduler(SCHED_FIFO, %ld) не удался: %s"),
			       rt_priority, strerror(errno));
	}
	if (setpriority(PRIO_PROCESS, 0, (int)nice_value) < 0 && errno != EPERM)
		m4_log(M4_LOG_WARN, M4_T("helper-exec: setpriority(%ld) failed: %s",
"helper-exec: setpriority(%ld) не удался: %s"), nice_value, strerror(errno));
	apply_cpu_affinity(cpu_list);

	/* `Environment=APTX_AD_START_MUTE_PKTS=0` in the old unit file, now
	 * config-driven -- always set (never left to the loader's
	 * own compiled default of 20) so behaviour matches the existing live
	 * unit exactly unless the operator changes `[helper] start-mute-pkts`. */
	setenv_int("APTX_AD_START_MUTE_PKTS", start_mute_pkts);
	apply_loader_env(loader_env);

	{
		char *exec_argv[4];

		exec_argv[0] = (char *)qemu_path;
		exec_argv[1] = (char *)loader_path;
		exec_argv[2] = (char *)capi_path;
		exec_argv[3] = NULL;
		execv(qemu_path, exec_argv);
	}
	/* execv() only returns on failure. */
	m4_log(M4_LOG_ERR, M4_T("helper-exec: execv(%s) failed: %s",
"helper-exec: execv(%s) не удался: %s"), qemu_path, strerror(errno));
	fprintf(stderr, "momentum4 helper-exec: execv(%s): %s\n", qemu_path, strerror(errno));
	return 127;
}
