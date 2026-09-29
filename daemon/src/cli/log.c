/* cli/log.c -- see log.h. */
#include "log.h"

#include "../../../common/m4-i18n.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-journal.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void print_usage(FILE *out)
{
	fprintf(out,
		M4_T("usage: momentum4 log [-n N] [--follow]\n"
		"  -n N        show the last N entries (default: 50)\n"
		"  --follow    keep showing new entries (Ctrl+C to exit)\n",
"usage: momentum4 log [-n N] [--follow]\n"
		"  -n N        показать последние N записей (по умолчанию 50)\n"
		"  --follow    продолжать показывать новые записи (Ctrl+C для выхода)\n"));
}

/* Matches every journal entry this project's own processes ever write
 * (util/log.c's m4_log() -> sd_journal_print(), COMM = argv[0]'s basename:
 * "momentum4" for every CLI invocation, "momentum4d" only if something
 * literally execs the binary under that name -- main.c's own argv[0]==
 * "momentum4d" special case is reached via a symlink/copy, not exec, so in
 * practice COMM is "momentum4" even for the daemon; both names matched
 * anyway in case that ever changes). */
static int add_matches(sd_journal *j)
{
	int r = sd_journal_add_match(j, "_COMM=momentum4", 0);

	if (r < 0)
		return r;
	r = sd_journal_add_disjunction(j);
	if (r < 0)
		return r;
	return sd_journal_add_match(j, "_COMM=momentum4d", 0);
}

static void print_entry(sd_journal *j)
{
	const void *data;
	size_t len;
	uint64_t usec = 0;
	char timebuf[32] = "?";
	const char *msg = "";
	size_t msg_len = 0;

	if (sd_journal_get_realtime_usec(j, &usec) >= 0) {
		time_t t = (time_t)(usec / 1000000);
		struct tm tm_buf;

		if (localtime_r(&t, &tm_buf) != NULL)
			strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_buf);
	}
	if (sd_journal_get_data(j, "MESSAGE", &data, &len) >= 0) {
		const char *eq = memchr(data, '=', len);

		if (eq != NULL) {
			msg = eq + 1;
			msg_len = len - (size_t)(eq + 1 - (const char *)data);
		}
	}
	printf("%s %.*s\n", timebuf, (int)msg_len, msg);
}

int m4_cli_log_run(int argc, char **argv)
{
	int n = 50;
	bool follow = false;
	sd_journal *j = NULL;
	int r;
	int i;
	int shown = 0;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			n = atoi(argv[++i]);
			if (n <= 0)
				n = 50;
		} else if (strcmp(argv[i], "--follow") == 0) {
			follow = true;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(stdout);
			return 0;
		} else {
			fprintf(stderr, M4_T("momentum4 log: unknown argument: %s\n",
"momentum4 log: неизвестный аргумент: %s\n"), argv[i]);
			print_usage(stderr);
			return 2;
		}
	}

	r = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY);
	if (r < 0) {
		fprintf(stderr, "momentum4 log: sd_journal_open: %s\n", strerror(-r));
		return 1;
	}
	if (add_matches(j) < 0) {
		fprintf(stderr, M4_T("momentum4 log: sd_journal_add_match failed\n",
"momentum4 log: sd_journal_add_match не удался\n"));
		sd_journal_close(j);
		return 1;
	}

	/* Last N: seek to the tail, then step backward N times collecting
	 * cursors is overkill for a CLI tool -- simpler to seek tail, walk
	 * backward n times noting we have entries, then walk forward printing.
	 * sd_journal_previous() N times from the tail, then sd_journal_next()
	 * (count+1) times printing -- avoids storing anything. */
	if (sd_journal_seek_tail(j) < 0) {
		fprintf(stderr, M4_T("momentum4 log: sd_journal_seek_tail failed\n",
"momentum4 log: sd_journal_seek_tail не удался\n"));
		sd_journal_close(j);
		return 1;
	}
	{
		int back = 0;

		while (back < n) {
			r = sd_journal_previous(j);
			if (r <= 0)
				break;
			back++;
		}
	}
	while (sd_journal_next(j) > 0) {
		print_entry(j);
		shown++;
	}
	if (shown == 0)
		printf(M4_T("(no momentum4 entries found in the journal)\n",
"(записей momentum4 в журнале не найдено)\n"));

	if (follow) {
		signal(SIGINT, on_sigint);
		signal(SIGTERM, on_sigint);
		while (!g_stop) {
			r = sd_journal_wait(j, 1000000 /* 1s, so Ctrl+C is noticed promptly */);
			if (r < 0)
				break;
			if (r == SD_JOURNAL_APPEND || r == SD_JOURNAL_INVALIDATE) {
				while (sd_journal_next(j) > 0)
					print_entry(j);
				fflush(stdout);
			}
		}
	}

	sd_journal_close(j);
	return 0;
}
