/* cli/repair_a2dp.c -- see repair_a2dp.h. */
#include "repair_a2dp.h"

#include <stdio.h>
#include <string.h>

#include "../bt/bt_ops.h"
#include "../core/graph_ops.h"
#include "../ipc/ctl.h"
#include "../pw/repair_a2dp.h"
#include "../util/config.h"
#include "../../../common/m4-i18n.h"

/* Generous relative to the daemon-side operation this wraps
 * (m4_repair_a2dp_run(): up to connect_timeout_ms (task default 15s) +
 * enum_wait_ms (task-specified 10s) + confirm_timeout_ms (daemon's own
 * confirm_timeout_ms, currently 10s, cli/daemon_cmd.c) sequentially, worst
 * case). Comfortably above that sum so a genuinely slow-but-working repair
 * is never mistaken for "daemon unreachable" and does not spuriously fall
 * through to the direct path (which would then race the daemon's own,
 * still-in-flight attempt). Was a compiled #define (40000); now `[cli]
 * repair-a2dp-timeout-ms` (configurable). */

/* Direct-path bounds -- same defaults pw/repair_a2dp.h documents, spelled
 * out here so `momentum4 repair-a2dp --help`/a reader does not have to go
 * looking for them. M4_CLI_REPAIR_A2DP_OPEN_SETTLE_MS was a compiled
 * #define (3000); now `[cli] repair-a2dp-poll-ms`. */
#define M4_CLI_REPAIR_A2DP_CONNECT_TIMEOUT_MS M4_REPAIR_A2DP_DEFAULT_CONNECT_TIMEOUT_MS
#define M4_CLI_REPAIR_A2DP_ENUM_WAIT_MS M4_REPAIR_A2DP_DEFAULT_ENUM_WAIT_MS
#define M4_CLI_REPAIR_A2DP_CONFIRM_TIMEOUT_MS M4_REPAIR_A2DP_DEFAULT_CONFIRM_TIMEOUT_MS

static int try_daemon(void)
{
	char sock_path[512];
	char response[M4_CTL_MAX_RESPONSE];

	if (m4_config_ctl_socket_path(sock_path, sizeof(sock_path)) == NULL)
		return -1;
	if (m4_ctl_client_request(sock_path, "REPAIR_A2DP", response, sizeof(response),
				   (int)m4_conf_get_int(m4_config(), "cli", "repair-a2dp-timeout-ms", 40000)) != 0)
		return -1;

	if (strncmp(response, "OK", 2) == 0) {
		char message[M4_CTL_MAX_RESPONSE];

		if (m4_ctl_response_field(response, "message", message, sizeof(message)))
			printf("%s\n", message);
		return 0;
	}
	fprintf(stderr, "%s", response);
	return 4;
}

static int run_direct(void)
{
	const char *mac = m4_config_mac_address();
	const char *bt_adapter = m4_config_bt_adapter();
	struct m4_repair_a2dp_result r;

	printf(M4_T("momentum4 repair-a2dp: daemon unavailable -- restoring a2dp-sink directly\n",
"momentum4 repair-a2dp: демон недоступен -- восстанавливаю a2dp-sink напрямую\n"));
	r = m4_repair_a2dp_run_standalone(&m4_graph_ops_pw, &m4_bt_ops_sdbus, mac, bt_adapter,
					   (int)m4_conf_get_int(m4_config(), "cli", "repair-a2dp-poll-ms", 3000),
					   M4_CLI_REPAIR_A2DP_CONNECT_TIMEOUT_MS,
					   M4_CLI_REPAIR_A2DP_ENUM_WAIT_MS,
					   M4_CLI_REPAIR_A2DP_CONFIRM_TIMEOUT_MS);
	if (r.status == M4_REPAIR_A2DP_OK) {
		printf("%s\n", r.message);
		return 0;
	}
	fprintf(stderr, "ERR %s\n", r.message);
	return 4;
}

int m4_cli_repair_a2dp_run(int argc, char **argv)
{
	int rc;

	(void)argv;
	if (argc != 0) {
		fprintf(stderr, "usage: momentum4 repair-a2dp\n");
		return 2;
	}

	/* try_daemon() returns -1 for "could not talk to the daemon at all"
	 * (no runtime dir, ctl.sock not listening, ...) -- an ENTIRELY
	 * expected, common outcome here (unlike cli/call.c, which treats it
	 * as a hard error): an emergency-rollback caller may stop
	 * momentum4d.service as one of its very first steps, before it ever
	 * reaches the point that calls this command, so "no daemon" is the
	 * NORMAL path from that caller, not an edge case. */
	rc = try_daemon();
	if (rc >= 0)
		return rc;

	return run_direct();
}
