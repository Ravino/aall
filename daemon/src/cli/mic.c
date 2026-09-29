/* cli/mic.c -- see mic.h. */
#include "mic.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../ipc/ctl.h"
#include "../util/config.h"
#include "../../../common/m4-i18n.h"

/* Same budget as cli/call.c's `[cli] call-timeout-ms` -- unlike `call on/off`,
 * mic on/off/toggle never waits on a profile-confirm (daemon/daemon.c's
 * daemon_execute_mic() bounds its own confirm-poll at 0.3s), so in practice
 * this always returns almost immediately; the generous timeout only guards
 * against a genuinely stuck daemon (busy with a concurrent, slower `call`
 * action -- an "ERR, another action already in progress" reply), same as every other
 * ctl.sock CLI command in this directory. There is no separate `[cli]
 * mic-timeout-ms` key in the registry -- deliberately reuses call-timeout-ms
 * (same reasoning documented here). */

static void print_no_daemon_hint(void)
{
	fprintf(stderr,
		M4_T("momentum4 mic: the momentum4d daemon is unavailable (not running, or ctl.sock is not responding).\n",
"momentum4 mic: демон momentum4d недоступен (не запущен, или ctl.sock не отвечает).\n"));
}

static int run_request(const char *req)
{
	char sock_path[512];
	char response[M4_CTL_MAX_RESPONSE];

	if (m4_config_ctl_socket_path(sock_path, sizeof(sock_path)) == NULL) {
		print_no_daemon_hint();
		return 3;
	}
	if (m4_ctl_client_request(sock_path, req, response, sizeof(response),
				   (int)m4_conf_get_int(m4_config(), "cli", "call-timeout-ms", 45000)) < 0) {
		print_no_daemon_hint();
		return 3;
	}

	if (strncmp(response, "OK", 2) == 0) {
		char muted[16], have_route[16], volume[16];

		if (m4_ctl_response_field(response, "have_route", have_route, sizeof(have_route))) {
			/* MIC STATUS response (daemon.c's handle_mic()): have_route=
			 * always present there, never on the on/off/toggle response. */
			if (strcmp(have_route, "true") != 0) {
				printf(M4_T("microphone: no route (not in a call)\n",
"микрофон: нет маршрута (не в звонке)\n"));
			} else {
				m4_ctl_response_field(response, "muted", muted, sizeof(muted));
				printf(M4_T("microphone: %s",
"микрофон: %s"), strcmp(muted, "true") == 0 ? M4_T("off",
"выключен") : M4_T("on",
"включён"));
				if (m4_ctl_response_field(response, "volume", volume, sizeof(volume)))
					printf(M4_T(" (volume %s)",
" (громкость %s)"), volume);
				printf("\n");
			}
		} else if (m4_ctl_response_field(response, "muted", muted, sizeof(muted))) {
			printf(M4_T("microphone %s\n",
"микрофон %s\n"), strcmp(muted, "true") == 0 ? M4_T("off",
"выключен") : M4_T("on",
"включён"));
		} else {
			printf("OK\n");
		}
		return 0;
	}

	fprintf(stderr, "%s", response);
	return 4;
}

int m4_cli_mic_run(int argc, char **argv)
{
	if (argc != 1) {
		fprintf(stderr, "usage: momentum4 mic on|off|toggle|status\n");
		return 2;
	}
	if (strcmp(argv[0], "on") == 0)
		return run_request("MIC ON");
	if (strcmp(argv[0], "off") == 0)
		return run_request("MIC OFF");
	if (strcmp(argv[0], "toggle") == 0)
		return run_request("MIC TOGGLE");
	if (strcmp(argv[0], "status") == 0)
		return run_request("MIC STATUS");

	fprintf(stderr, M4_T("momentum4 mic: unknown argument: %s (expected on|off|toggle|status)\n",
"momentum4 mic: неизвестный аргумент: %s (ожидается on|off|toggle|status)\n"), argv[0]);
	return 2;
}
