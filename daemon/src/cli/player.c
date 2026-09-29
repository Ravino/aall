/* cli/player.c -- see player.h. */
#include "player.h"

#include <stdio.h>
#include <string.h>

#include "../ipc/ctl.h"
#include "../util/config.h"
#include "../../../common/m4-i18n.h"

static int run_request(const char *req)
{
	char sock_path[512];
	char response[M4_CTL_MAX_RESPONSE];

	if (m4_config_ctl_socket_path(sock_path, sizeof(sock_path)) == NULL ||
	    m4_ctl_client_request(sock_path, req, response, sizeof(response),
				   (int)m4_conf_get_int(m4_config(), "cli", "player-timeout-ms", 2000)) < 0) {
		fprintf(stderr, M4_T("momentum4: the momentum4d daemon is unavailable (not running, or ctl.sock is not responding).\n",
"momentum4: демон momentum4d недоступен (не запущен, или ctl.sock не отвечает).\n"));
		return 3;
	}
	if (strncmp(response, "OK", 2) == 0) {
		/* print the key=value lines as they are */
		const char *body = strchr(response, '\n');

		fputs(body != NULL ? body + 1 : "", stdout);
		return 0;
	}
	fprintf(stderr, "%s", response);
	return 4;
}

int m4_cli_player_run(int argc, char **argv)
{
	char req[64];

	if (argc == 0)
		return run_request("PLAYER");
	if (argc == 1 && strcmp(argv[0], "resync") == 0)
		return run_request("PLAYER RESYNC");
	if (argc == 2 && strcmp(argv[0], "status") == 0) {
		snprintf(req, sizeof(req), "PLAYER STATUS %s", argv[1]);
		return run_request(req);
	}
	fprintf(stderr, "usage: momentum4 player [status stopped|playing|paused|follow | resync]\n");
	return 2;
}

int m4_cli_reload_run(int argc, char **argv)
{
	(void)argv;
	if (argc != 0) {
		fprintf(stderr, "usage: momentum4 reload\n");
		return 2;
	}
	return run_request("RELOAD");
}
