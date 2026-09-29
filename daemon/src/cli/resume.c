/* cli/resume.c -- see resume.h. */
#include "resume.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../util/config.h"
#include "../../../common/m4-i18n.h"

int m4_cli_resume_run(int argc, char **argv)
{
	char runtime_dir[480];
	char path[512];

	(void)argv;
	if (argc != 0) {
		fprintf(stderr, "usage: momentum4 resume\n");
		return 2;
	}

	if (m4_config_runtime_dir(runtime_dir, sizeof(runtime_dir)) == NULL) {
		fprintf(stderr, M4_T("momentum4 resume: failed to determine the runtime directory "
				 "(no $XDG_RUNTIME_DIR)\n",
"momentum4 resume: не удалось определить runtime-каталог "
				 "(нет $XDG_RUNTIME_DIR)\n"));
		return 1;
	}
	if (snprintf(path, sizeof(path), "%s/safe-latch", runtime_dir) >= (int)sizeof(path)) {
		fprintf(stderr, M4_T("momentum4 resume: path too long\n",
"momentum4 resume: путь слишком длинный\n"));
		return 1;
	}

	if (unlink(path) == 0) {
		printf(M4_T("The safe-latch has been released. The daemon (if running) can change the M4 profile again.\n",
"Защёлка safe-latch снята. Демон (если запущен) снова может менять профиль M4.\n"));
		return 0;
	}
	if (errno == ENOENT) {
		printf(M4_T("The safe-latch was not set -- nothing changed.\n",
"Защёлка safe-latch не была установлена -- ничего не изменилось.\n"));
		return 0;
	}
	fprintf(stderr, M4_T("momentum4 resume: failed to remove %s: %s\n",
"momentum4 resume: не удалось удалить %s: %s\n"), path, strerror(errno));
	return 1;
}
