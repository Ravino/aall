#include "status.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "../core/profile_rules.h"
#include "../ipc/ctl.h"
#include "../metrics/stats.h"
#include "../pw/graph_pw.h"
#include "../util/config.h"
#include "../util/log.h"
#include "../util/rt_status.h"
#include "../../../common/m4-i18n.h"

/* How long m4_pw_query_snapshot() is allowed to block (`[cli]
 * status-timeout-ms`, was a compiled #define 700). Generous relative to a
 * local Unix-socket registry roundtrip (typically single-digit ms), tight
 * enough that a stuck/overloaded PipeWire still leaves `status` well under
 * a sub-second response target.
 *
 * How long the daemon-first path waits for ctl.sock
 * before falling back to the direct PipeWire read below (`[cli]
 * status-poll-ms`, was a compiled #define 300) -- short on purpose: this is
 * a LOCAL Unix socket to a process on the same machine that either answers
 * near-instantly (cached state, no round trip -- see daemon/daemon.c's
 * handle_get()) or is not running/not listening at all (connect() fails
 * immediately, this timeout is never actually spent waiting in that common
 * case either). */

/* Everything `momentum4 status` shows that ONLY the daemon knows (mode,
 * automode/call bookkeeping, safe-latch) -- separate from struct
 * m4_pw_snapshot, which try_daemon_status() below also partially fills in
 * from the SAME ctl.sock round trip (prefer the
 * daemon's cached graph state over a fresh PipeWire query when a daemon is
 * reachable at all, since it is strictly cheaper and just as current --
 * daemon/daemon.c's graph_ops callback updates it on every change). */
struct m4_status_daemon_extra {
	bool reachable;
	char mode[16];
	bool safe_latched;
	bool call_active;
	char call_reason[16];
	bool frozen;
};

/* Tries `GET` over ctl.sock. On success, fills `pw` with the subset of
 * struct m4_pw_snapshot the daemon's cached struct m4_graph_state can
 * provide (m4_present/m4_connected/profile_name/mode -- NOT codec: pw/
 * graph_pw.c's struct graph_ops implementation never reads the bluez5
 * Sink Node's api.bluez5.codec at all, only the Device's Profile/
 * EnumProfile,
 * deliberately, rather than guessing) and `extra` with the daemon-only
 * fields, returning true. Returns false (both output structs left
 * zeroed) if the daemon is not reachable within `[cli] status-poll-ms`,
 * in which case the caller falls back to m4_pw_query_snapshot(). */
static bool try_daemon_status(struct m4_pw_snapshot *pw, struct m4_status_daemon_extra *extra)
{
	char sock_path[512];
	char response[M4_CTL_MAX_RESPONSE];
	char field[64];

	memset(pw, 0, sizeof(*pw));
	memset(extra, 0, sizeof(*extra));

	if (m4_config_ctl_socket_path(sock_path, sizeof(sock_path)) == NULL)
		return false;
	if (m4_ctl_client_request(sock_path, "GET", response, sizeof(response),
				   (int)m4_conf_get_int(m4_config(), "cli", "status-poll-ms", 300)) != 0)
		return false;
	if (strncmp(response, "OK", 2) != 0)
		return false;

	if (m4_ctl_response_field(response, "m4_present", field, sizeof(field)))
		pw->m4_present = (strcmp(field, "true") == 0);
	if (m4_ctl_response_field(response, "m4_connected", field, sizeof(field)))
		pw->m4_connected = (strcmp(field, "true") == 0);
	if (m4_ctl_response_field(response, "profile", field, sizeof(field)) && field[0] != '\0') {
		pw->have_profile = true;
		snprintf(pw->profile_name, sizeof(pw->profile_name), "%s", field);
		pw->mode = m4_profile_mode(field);
	}

	extra->reachable = true;
	m4_ctl_response_field(response, "mode", extra->mode, sizeof(extra->mode));
	if (m4_ctl_response_field(response, "safe_latched", field, sizeof(field)))
		extra->safe_latched = (strcmp(field, "true") == 0);
	if (m4_ctl_response_field(response, "call_active", field, sizeof(field)))
		extra->call_active = (strcmp(field, "true") == 0);
	m4_ctl_response_field(response, "call_reason", extra->call_reason, sizeof(extra->call_reason));
	if (m4_ctl_response_field(response, "frozen", field, sizeof(field)))
		extra->frozen = (strcmp(field, "true") == 0);
	return true;
}

/* RT status: what `momentum4 status` shows about the
 * ACTUALLY obtained RT priorities -- the bridge IO thread (stats.json's own
 * `bridge_rt_policy`/`_prio`, already loaded by the caller below for the
 * existing "Bridge: RT ..." line (Russian text) -- see util/rt_status.h's own top comment
 * for why this file cannot read the bridge's OWN scheduling any other way)
 * and the qemu-hexagon HELPER PROCESS (util/rt_status.h's /proc-only,
 * read-only PID discovery) -- compared against `[adaptive]
 * io-rt-priority`/`[helper] rt-priority`, the SAME comparison
 * cli/doctor.c's own check_rt_status() makes (m4_rt_status_matches(), the
 * one shared pure decision function -- both commands agree, never two
 * possibly-diverging opinions). */
struct m4_status_rt_extra {
	int configured_io_rt_priority;
	bool bridge_match; /* only meaningful when the caller's own stats_ok is true */

	bool helper_found;
	pid_t helper_pid;
	char helper_policy[16];
	int helper_rt_priority;
	int configured_helper_rt_priority;
	bool helper_match;
};

static void gather_rt_extra(bool stats_ok, const struct m4_stats *st, struct m4_status_rt_extra *out)
{
	const m4_conf_t *conf = m4_config();
	pid_t pid;

	memset(out, 0, sizeof(*out));
	out->configured_io_rt_priority = (int)m4_conf_get_int(conf, "adaptive", "io-rt-priority", 70);
	out->configured_helper_rt_priority = (int)m4_conf_get_int(conf, "helper", "rt-priority", 10);
	out->bridge_match = stats_ok && st->alive &&
			    m4_rt_status_matches(out->configured_io_rt_priority, st->bridge_rt_policy,
						  st->bridge_rt_prio);

	pid = m4_rt_status_find_helper_pid(m4_conf_get_string(conf, "helper", "qemu"));
	if (pid > 0) {
		struct m4_rt_proc_status ps;

		m4_rt_status_query_pid(pid, &ps);
		out->helper_found = true;
		out->helper_pid = pid;
		snprintf(out->helper_policy, sizeof(out->helper_policy), "%s",
			 m4_rt_status_policy_name(ps.sched_policy));
		out->helper_rt_priority = ps.rt_priority;
		out->helper_match = m4_rt_status_matches(out->configured_helper_rt_priority, out->helper_policy,
							  ps.rt_priority);
	}
}

/* Minimal JSON string escaper: device/profile descriptions come from BlueZ
 * (ultimately: whatever alias the phone/headset set, e.g. via a companion
 * app) and must not be trusted to be free of '"'/'\\'/control characters
 * when embedded in --json output. Truncates rather than overflowing `out`;
 * always NUL-terminates if out_cap > 0. */
static void json_escape(const char *in, char *out, size_t out_cap)
{
	size_t o = 0;

	if (out_cap == 0)
		return;
	if (in == NULL) {
		out[0] = '\0';
		return;
	}
	for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
		const char *rep = NULL;
		char rep_buf[7];

		switch (*p) {
		case '"': rep = "\\\""; break;
		case '\\': rep = "\\\\"; break;
		case '\n': rep = "\\n"; break;
		case '\r': rep = "\\r"; break;
		case '\t': rep = "\\t"; break;
		default:
			if (*p < 0x20) {
				snprintf(rep_buf, sizeof(rep_buf), "\\u%04x", *p);
				rep = rep_buf;
			}
			break;
		}
		if (rep != NULL) {
			size_t rl = strlen(rep);
			if (o + rl >= out_cap)
				break;
			memcpy(out + o, rep, rl);
			o += rl;
		} else {
			if (o + 1 >= out_cap)
				break;
			out[o++] = (char)*p;
		}
	}
	out[o] = '\0';
}

static void print_json(const char *mac, bool pw_ok, const struct m4_pw_snapshot *pw,
			bool stats_ok, const struct m4_stats *st,
			const struct m4_status_daemon_extra *daemon,
			const struct m4_status_rt_extra *rt)
{
	char esc1[256], esc2[256], esc3[256];

	printf("{");
	printf("\"mac_address\":\"%s\",", mac);
	printf("\"pipewire\":{");
	printf("\"reachable\":%s", pw_ok ? "true" : "false");
	if (pw_ok) {
		json_escape(pw->device_description, esc1, sizeof(esc1));
		json_escape(pw->profile_name, esc2, sizeof(esc2));
		json_escape(pw->profile_description, esc3, sizeof(esc3));
		printf(",\"m4_present\":%s", pw->m4_present ? "true" : "false");
		printf(",\"m4_connected\":%s", pw->m4_connected ? "true" : "false");
		printf(",\"device_description\":\"%s\"", esc1);
		printf(",\"mode\":\"%s\"", m4_mode_token(pw->mode));
		printf(",\"profile_name\":\"%s\"", esc2);
		printf(",\"profile_description\":\"%s\"", esc3);
		printf(",\"enum_profile_count\":%u", pw->enum_profile_count);
		printf(",\"codec\":%s", pw->have_codec ? "\"" : "null");
		if (pw->have_codec) {
			char esc4[64];
			json_escape(pw->codec, esc4, sizeof(esc4));
			printf("%s\"", esc4);
		}
		printf(",\"timed_out\":%s", pw->timed_out ? "true" : "false");
	}
	printf("},");

	printf("\"helper\":{");
	printf("\"stats_available\":%s", stats_ok ? "true" : "false");
	if (stats_ok) {
		printf(",\"alive\":%s", st->alive ? "true" : "false");
		printf(",\"mode\":\"%s\"", st->mode);
		printf(",\"uptime_ms\":%.0f", st->uptime_ms);
		printf(",\"frames_produced\":%u", st->frames_produced);
		printf(",\"encode_calls\":%u", st->encode_calls);
		printf(",\"helper_late\":%u", st->helper_late);
		printf(",\"stall_events\":%u", st->stall_events);
		printf(",\"helper_restarts\":%u", st->helper_restarts);
		printf(",\"ring_in\":%s", st->have_ring_in ? "" : "null");
		if (st->have_ring_in)
			printf("%u", st->ring_in);
		printf(",\"ring_in_cap\":%u", st->ring_in_cap);
		printf(",\"ring_out\":%s", st->have_ring_out ? "" : "null");
		if (st->have_ring_out)
			printf("%u", st->ring_out);
		printf(",\"ring_out_cap\":%u", st->ring_out_cap);
		printf(",\"bridge_rt_policy\":\"%s\"", st->bridge_rt_policy);
		printf(",\"bridge_rt_prio\":%d", st->bridge_rt_prio);
		printf(",\"bridge_rt_priority_configured\":%d", rt->configured_io_rt_priority);
		printf(",\"bridge_rt_priority_match\":%s", rt->bridge_match ? "true" : "false");
	}
	printf("},");

	printf("\"helper_process\":{");
	printf("\"found\":%s", rt->helper_found ? "true" : "false");
	if (rt->helper_found) {
		printf(",\"pid\":%d", (int)rt->helper_pid);
		printf(",\"rt_policy\":\"%s\"", rt->helper_policy);
		printf(",\"rt_priority\":%d", rt->helper_rt_priority);
		printf(",\"rt_priority_configured\":%d", rt->configured_helper_rt_priority);
		printf(",\"rt_priority_match\":%s", rt->helper_match ? "true" : "false");
	}
	printf("},");

	printf("\"daemon\":{");
	printf("\"reachable\":%s", daemon->reachable ? "true" : "false");
	if (daemon->reachable) {
		printf(",\"mode\":\"%s\"", daemon->mode);
		printf(",\"safe_latched\":%s", daemon->safe_latched ? "true" : "false");
		printf(",\"call_active\":%s", daemon->call_active ? "true" : "false");
		printf(",\"call_reason\":\"%s\"", daemon->call_reason);
		printf(",\"frozen\":%s", daemon->frozen ? "true" : "false");
	}
	printf("}");

	printf("}\n");
}

static void print_human(bool pw_ok, const struct m4_pw_snapshot *pw,
			 bool stats_ok, const struct m4_stats *st,
			 const struct m4_status_daemon_extra *daemon,
			 const struct m4_status_rt_extra *rt)
{
	if (!pw_ok) {
		printf(M4_T("PipeWire: unavailable\n",
"PipeWire: недоступен\n"));
	} else if (!pw->m4_present) {
		printf(M4_T("M4: not found in PipeWire (no bluez5 card with this address)\n",
"M4: не найдены в PipeWire (нет карты bluez5 с этим адресом)\n"));
	} else if (!pw->m4_connected) {
		printf(M4_T("M4: card present, but the headset is not connected\n",
"M4: карта есть, но наушники не подключены\n"));
	} else {
		printf(M4_T("M4: connected, mode %s",
"M4: подключены, режим %s"), m4_mode_human(pw->mode));
		if (pw->mode == M4_MODE_UNKNOWN && pw->profile_name[0] != '\0')
			printf(" (\"%s\")", pw->profile_name);
		if (pw->have_codec)
			printf(M4_T(", codec %s",
", кодек %s"), pw->codec);
		printf("\n");
	}
	if (pw_ok && pw->timed_out)
		printf(M4_T("PipeWire: reply did not complete within the time allotted\n",
"PipeWire: ответ пришёл не полностью за отведённое время\n"));

	if (daemon->reachable) {
		printf(M4_T("Daemon: mode %s%s, call %s (%s)%s\n",
"Демон: режим %s%s, звонок %s (%s)%s\n"), daemon->mode,
		       daemon->safe_latched ? M4_T(", safe-latch LATCHED",
", ЗАЩЁЛКА safe-latch") : "",
		       daemon->call_active ? M4_T("active",
"активен") : M4_T("not active",
"не активен"),
		       daemon->call_reason[0] != '\0' ? daemon->call_reason : "none",
		       daemon->frozen ? M4_T(", FROZEN (looks like flapping)",
", ЗАМОРОЖЕН (похоже на флаппинг)") : "");
	} else {
		printf(M4_T("Daemon: not running (momentum4d.service) -- shown via direct read\n",
"Демон: не запущен (momentum4d.service) -- показано прямым чтением\n"));
	}

	if (!stats_ok) {
		printf(M4_T("Helper: no data (stats file unavailable)\n",
"Helper: нет данных (файл статистики недоступен)\n"));
		return;
	}
	printf(M4_T("Helper: %s, mode %s, frames %u, late %u, restarts %u, underruns %u\n",
"Helper: %s, режим %s, кадров %u, опозданий %u, рестартов %u, недоборов %u\n"),
	       st->alive ? M4_T("alive",
"жив") : M4_T("not responding",
"не отвечает"), st->mode[0] != '\0' ? st->mode : "?",
	       st->frames_produced, st->helper_late, st->helper_restarts, st->stall_events);
	printf(M4_T("Rings: in ",
"Кольца: вход "));
	if (st->have_ring_in)
		printf("%u", st->ring_in);
	else
		printf("?");
	printf(M4_T("/%u, out ",
"/%u, выход "), st->ring_in_cap);
	if (st->have_ring_out)
		printf("%u", st->ring_out);
	else
		printf("?");
	printf("/%u\n", st->ring_out_cap);
	if (st->bridge_rt_policy[0] != '\0') {
		printf(M4_T("Bridge: RT %s/%d",
"Мост: RT %s/%d"), st->bridge_rt_policy, st->bridge_rt_prio);
		if (!rt->bridge_match)
			printf(M4_T(" (EXPECTED %s -- [adaptive] io-rt-priority=%d, check LimitRTPRIO/rtkit)",
" (ОЖИДАЕТСЯ %s -- [adaptive] io-rt-priority=%d, проверьте LimitRTPRIO/rtkit)"),
			       rt->configured_io_rt_priority > 0 ? M4_T("SCHED_FIFO with the same number",
"SCHED_FIFO с тем же числом") : "SCHED_OTHER",
			       rt->configured_io_rt_priority);
		printf("\n");
	}
	if (rt->helper_found) {
		printf(M4_T("Helper process (qemu-hexagon, pid %d): RT %s/%d",
"Helper-процесс (qemu-hexagon, pid %d): RT %s/%d"), (int)rt->helper_pid, rt->helper_policy,
		       rt->helper_rt_priority);
		if (!rt->helper_match)
			printf(M4_T(" (EXPECTED %s -- [helper] rt-priority=%d, check the unit's LimitRTPRIO/rtkit)",
" (ОЖИДАЕТСЯ %s -- [helper] rt-priority=%d, проверьте LimitRTPRIO юнита/rtkit)"),
			       rt->configured_helper_rt_priority > 0 ? M4_T("SCHED_FIFO with the same number",
"SCHED_FIFO с тем же числом") : "SCHED_OTHER",
			       rt->configured_helper_rt_priority);
		printf("\n");
	}
}

int m4_cli_status_run(int argc, char **argv)
{
	bool json = false;
	const char *mac;
	struct m4_pw_snapshot pw_snap;
	struct m4_stats stats;
	struct m4_status_daemon_extra daemon;
	struct m4_status_rt_extra rt;
	char stats_path[512];
	bool pw_ok, stats_ok;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0) {
			json = true;
		} else {
			fprintf(stderr, M4_T("momentum4 status: unknown argument: %s\n",
"momentum4 status: неизвестный параметр: %s\n"), argv[i]);
			return 2;
		}
	}

	mac = m4_config_mac_address();

	/* Prefer the daemon's cached state (cheaper, same
	 * information for the fields it tracks) if momentum4d is reachable;
	 * fall back to the direct PipeWire read already
	 * implemented otherwise -- status works without the daemon. */
	pw_ok = try_daemon_status(&pw_snap, &daemon);
	if (!pw_ok)
		pw_ok = (m4_pw_query_snapshot(mac, (int)m4_conf_get_int(m4_config(), "cli", "status-timeout-ms", 700), &pw_snap) == M4_PW_OK);

	stats_ok = false;
	if (m4_config_stats_path(stats_path, sizeof(stats_path)) != NULL)
		stats_ok = (m4_stats_load(stats_path, &stats) == 0);
	if (!stats_ok)
		memset(&stats, 0, sizeof(stats));

	/* RT status: cheap (one optional stats.json re-read's worth of
	 * work, a /proc scan) -- always gathered, same "status is fast" budget
	 * every other field here already fits in. */
	gather_rt_extra(stats_ok, &stats, &rt);

	if (json)
		print_json(mac, pw_ok, &pw_snap, stats_ok, &stats, &daemon, &rt);
	else
		print_human(pw_ok, &pw_snap, stats_ok, &stats, &daemon, &rt);

	return 0;
}
