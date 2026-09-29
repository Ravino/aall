/* cli/mode.c -- see mode.h. */
#include "mode.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../ipc/ctl.h"
#include "../util/config.h"
#include "../../../common/m4-i18n.h"

static void print_no_daemon_hint(void)
{
	fprintf(stderr,
		M4_T("momentum4 mode: the momentum4d daemon is unavailable (not running, or ctl.sock is not responding).\n",
"momentum4 mode: демон momentum4d недоступен (не запущен, или ctl.sock не отвечает).\n"));
}

static int run_request(const char *req, bool want_value)
{
	char sock_path[512];
	char response[M4_CTL_MAX_RESPONSE];

	if (m4_config_ctl_socket_path(sock_path, sizeof(sock_path)) == NULL) {
		print_no_daemon_hint();
		return 3;
	}
	if (m4_ctl_client_request(sock_path, req, response, sizeof(response),
				   (int)m4_conf_get_int(m4_config(), "cli", "mode-timeout-ms", 1000)) < 0) {
		print_no_daemon_hint();
		return 3;
	}

	if (strncmp(response, "OK", 2) == 0) {
		char mode[32];

		if (want_value && m4_ctl_response_field(response, "mode", mode, sizeof(mode)))
			printf("%s\n", mode);
		return 0;
	}

	fprintf(stderr, "%s", response);
	return 4;
}

int m4_cli_mode_run(int argc, char **argv)
{
	if (argc == 0 || strcmp(argv[0], "get") == 0)
		return run_request("GET", true);
	if (argc != 1) {
		fprintf(stderr, "usage: momentum4 mode [observe|manual|auto|get]\n");
		return 2;
	}
	if (strcmp(argv[0], "observe") == 0 || strcmp(argv[0], "manual") == 0 ||
	    strcmp(argv[0], "auto") == 0) {
		char req[32];

		snprintf(req, sizeof(req), "MODE %s", argv[0]);
		return run_request(req, true);
	}
	fprintf(stderr, M4_T("momentum4 mode: unknown mode: %s (expected observe|manual|auto)\n",
"momentum4 mode: неизвестный режим: %s (ожидается observe|manual|auto)\n"),
		argv[0]);
	return 2;
}
