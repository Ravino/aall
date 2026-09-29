/* cli/call.c -- see call.h. */
#include "call.h"

#include <stdio.h>
#include <string.h>

#include "../ipc/ctl.h"
#include "../util/config.h"
#include "../../../common/m4-i18n.h"

/* was 1000 -- but the daemon only answers AFTER the
 * profile switch is confirmed (mSBC up to 10 s, + CVSD fallback 10 s, + mic
 * volume confirm), so a 1 s client timeout reported "daemon unavailable"
 * for every real switch while the switch itself went on. Was a compiled
 * #define (45000); now `[cli] call-timeout-ms` is configurable. */

static void print_no_daemon_hint(void)
{
	fprintf(stderr,
		M4_T("momentum4 call: the momentum4d daemon is unavailable (not running, or ctl.sock is not responding).\n"
		"This command only works through the daemon (observe/manual-mode phase).\n"
		"Start it with: systemctl --user start momentum4d.service\n",
"momentum4 call: демон momentum4d недоступен (не запущен, или ctl.sock не отвечает).\n"
		"Эта команда работает только через демон (режим наблюдения/ручной режим).\n"
		"Запустите его: systemctl --user start momentum4d.service\n"));
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
		char message[M4_CTL_MAX_RESPONSE];

		if (m4_ctl_response_field(response, "message", message, sizeof(message)))
			printf("%s\n", message);
		return 0;
	}

	fprintf(stderr, "%s", response);
	return 4;
}

int m4_cli_call_run(int argc, char **argv)
{
	if (argc != 1) {
		fprintf(stderr, "usage: momentum4 call on|off\n");
		return 2;
	}
	if (strcmp(argv[0], "on") == 0)
		return run_request("CALL ON");
	if (strcmp(argv[0], "off") == 0)
		return run_request("CALL OFF");

	fprintf(stderr, M4_T("momentum4 call: unknown argument: %s (expected on|off)\n",
"momentum4 call: неизвестный аргумент: %s (ожидается on|off)\n"), argv[0]);
	return 2;
}
