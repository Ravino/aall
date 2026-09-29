/* cli/stats.c -- see stats.h. Two output shapes (short summary /
 * --json / --history N), exit codes (0 read+parsed, 1 no file, 2
 * exists but did not parse), default paths (`[stats] path`/
 * `history-path`, common/m4-conf.c already expands ${XDG_RUNTIME_DIR}).
 */
#include "stats.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../../../common/m4-conf.h"
#include "../../../common/m4-i18n.h"
#include "../metrics/stats.h"
#include "../util/config.h"

#define M4_STATS_HISTORY_MAX_ROWS 4096 /* generous; matches M4_AH_STATS_HISTORY_MAX_LINES's
					 * own spirit -- a corrupt/huge history file is
					 * truncated to the last rows read, never OOM */

static void print_usage(FILE *out)
{
	fprintf(out,
		M4_T("usage: momentum4 stats [--json] [--history [N]] [--path FILE]\n"
		"  (no arguments)      short summary of aptX Adaptive bridge metrics\n"
		"  --json               raw JSON as-is (stats.json)\n"
		"  --history [N]        last N (default: 5) history entries\n"
		"  --path FILE           alternate path to stats.json (or, with --history, to the .jsonl)\n",
"usage: momentum4 stats [--json] [--history [N]] [--path FILE]\n"
		"  (без аргументов)     короткая сводка метрик моста aptX Adaptive\n"
		"  --json               сырой JSON как есть (stats.json)\n"
		"  --history [N]        последние N (по умолчанию 5) записей истории\n"
		"  --path FILE           другой путь к stats.json (или, с --history, к .jsonl)\n"));
}

static const char *fmt_uptime(double ms, char *buf, size_t cap)
{
	long total_s = (long)(ms / 1000.0);
	long h = total_s / 3600;
	long m = (total_s % 3600) / 60;
	long s = total_s % 60;

	if (h > 0)
		snprintf(buf, cap, M4_T("%ldh %ldm",
"%ld ч %ld мин"), h, m);
	else if (m > 0)
		snprintf(buf, cap, M4_T("%ldm %lds",
"%ld мин %ld с"), m, s);
	else
		snprintf(buf, cap, M4_T("%lds",
"%ld с"), s);
	return buf;
}

static const char *fmt_age(long long ts_unix_ms, char *buf, size_t cap)
{
	double now_s = (double)time(NULL);
	double age_s = now_s - (double)ts_unix_ms / 1000.0;

	if (age_s < 0)
		age_s = 0;
	if (age_s < 2)
		snprintf(buf, cap, M4_T("just now",
"только что"));
	else
		snprintf(buf, cap, M4_T("%.0f s ago",
"%.0f с назад"), age_s);
	return buf;
}

static void print_summary(const struct m4_stats *d)
{
	char uptime[32], age[32];

	fmt_uptime(d->uptime_ms, uptime, sizeof(uptime));
	fmt_age(d->ts_unix_ms, age, sizeof(age));

	printf(M4_T("aptX Adaptive %s: %s. Mode %s, adjust %d ms. Bridge uptime: %s. Updated: %s.",
"aptX Adaptive %s: %s. Режим %s, поправка %d мс. Время работы моста: %s. Обновлено: %s."),
	       d->mode[0] != '\0' ? d->mode : "?", d->alive ? M4_T("running",
"работает") : M4_T("SILENT (helper unavailable)",
"МОЛЧИТ (helper недоступен)"),
	       d->mode[0] != '\0' ? d->mode : "?", d->latency_adjust_ms, uptime, age);
	if (d->helper_restarts > 0)
		printf(M4_T(" Helper restarts: %u.",
" Перезапусков helper'а: %u."), d->helper_restarts);
	printf("\n");

	printf(M4_T("Frames: encoded %u, produced by the helper %u. Input drops: ring full %u, "
	       "trylock busy %u.\n",
"Кадры: закодировано %u, произведено helper'ом %u. Пропуски входа: заполнено кольцо %u, "
	       "занято потоком %u.\n"),
	       d->encode_calls, d->frames_produced, d->input_drops_ring_full, d->input_drops_trylock);
	printf(M4_T("Helper lateness: %u. Heartbeat triggers: late %u, stall %u, resync events %u.\n",
"Опоздания helper'а: %u. Триггеры heartbeat: опоздание %u, зависание %u, событий resync %u.\n"),
	       d->helper_late, d->heartbeat_late_triggers, d->stall_events, d->heartbeat_events);
	if (d->have_ring_in)
		printf(M4_T("Ring fill: in %u of %u, ",
"Заполненность колец: вход %u из %u, "), d->ring_in, d->ring_in_cap);
	else
		printf(M4_T("Ring fill: in unknown of %u, ",
"Заполненность колец: вход неизвестно из %u, "), d->ring_in_cap);
	if (d->have_ring_out)
		printf(M4_T("out %u of %u.\n",
"выход %u из %u.\n"), d->ring_out, d->ring_out_cap);
	else
		printf(M4_T("out unknown of %u.\n",
"выход неизвестно из %u.\n"), d->ring_out_cap);
}

static int cmd_json(const char *path)
{
	FILE *f = fopen(path, "re");
	char buf[8192];
	size_t n;

	if (f == NULL) {
		fprintf(stderr, M4_T("no metrics: %s (the codec has not run yet, or metrics are disabled)\n",
"метрики отсутствуют: %s (кодек ещё не запускался, или метрики выключены)\n"), path);
		return 1;
	}
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		fwrite(buf, 1, n, stdout);
	fclose(f);
	printf("\n");
	return 0;
}

static int cmd_history(const char *path, int n)
{
	FILE *f = fopen(path, "re");
	char line[4096];
	struct m4_stats rows[M4_STATS_HISTORY_MAX_ROWS];
	int count = 0;
	int i;

	if (f == NULL) {
		printf(M4_T("no history: %s (the codec has not run yet, or metrics are disabled)\n",
"история отсутствует: %s (кодек ещё не запускался или метрики выключены)\n"), path);
		return 1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		size_t len = strlen(line);
		struct m4_stats st;

		if (len == 0)
			continue;
		if (m4_stats_parse(line, len, &st) != 0)
			continue;
		if (count < M4_STATS_HISTORY_MAX_ROWS) {
			rows[count++] = st;
		} else {
			/* keep the ring "last N seen" by shifting -- history files
			 * are bounded (M4_AH_STATS_HISTORY_MAX_LINES) so this path
			 * is a defensive cap, not the common case. */
			memmove(&rows[0], &rows[1], sizeof(rows[0]) * (M4_STATS_HISTORY_MAX_ROWS - 1));
			rows[M4_STATS_HISTORY_MAX_ROWS - 1] = st;
		}
	}
	fclose(f);
	if (count == 0) {
		printf(M4_T("history is empty or could not be parsed: %s\n",
"история пуста или не разобралась: %s\n"), path);
		return 2;
	}
	if (n > count)
		n = count;
	for (i = count - n; i < count; i++) {
		char age[32];

		fmt_age(rows[i].ts_unix_ms, age, sizeof(age));
		printf("%s | alive=%s frames=%u late=%u restarts=%u ring_in=%u/%u ring_out=%u/%u\n", age,
		       rows[i].alive ? "True" : "False", rows[i].frames_produced, rows[i].helper_late,
		       rows[i].helper_restarts, rows[i].ring_in, rows[i].ring_in_cap, rows[i].ring_out,
		       rows[i].ring_out_cap);
	}
	return 0;
}

int m4_cli_stats_run(int argc, char **argv)
{
	bool want_json = false;
	int want_history = -1; /* -1 = not requested, else N */
	const char *path_override = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0) {
			want_json = true;
		} else if (strcmp(argv[i], "--history") == 0) {
			want_history = 5;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				char *end = NULL;
				long v = strtol(argv[i + 1], &end, 10);

				if (end != NULL && *end == '\0' && v > 0) {
					want_history = (int)v;
					i++;
				}
			}
		} else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
			path_override = argv[++i];
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(stdout);
			return 0;
		} else {
			fprintf(stderr, M4_T("momentum4 stats: unknown argument: %s\n",
"momentum4 stats: неизвестный аргумент: %s\n"), argv[i]);
			print_usage(stderr);
			return 2;
		}
	}

	if (want_history >= 0) {
		char path[512];

		if (path_override != NULL) {
			snprintf(path, sizeof(path), "%s", path_override);
		} else {
			const char *v = m4_conf_get_string(m4_config(), "stats", "history-path");

			snprintf(path, sizeof(path), "%s", v != NULL ? v : "");
		}
		return cmd_history(path, want_history);
	}

	{
		char path[512];
		struct m4_stats st;
		int rc;

		if (path_override != NULL) {
			snprintf(path, sizeof(path), "%s", path_override);
		} else if (m4_config_stats_path(path, sizeof(path)) == NULL) {
			fprintf(stderr, M4_T("momentum4 stats: failed to determine the stats.json path "
					 "(no $XDG_RUNTIME_DIR)\n",
"momentum4 stats: не удалось определить путь stats.json "
					 "(нет $XDG_RUNTIME_DIR)\n"));
			return 1;
		}

		if (want_json)
			return cmd_json(path);

		rc = m4_stats_load(path, &st);
		if (rc == -ENOENT) {
			printf(M4_T("no metrics: %s (the codec has not run yet, the helper never connected, "
			       "or metrics are disabled)\n",
"метрики отсутствуют: %s (кодек ещё не запускался, helper не подключался, "
			       "или метрики выключены)\n"),
			       path);
			return 1;
		}
		if (rc != 0) {
			printf(M4_T("failed to parse %s (try again -- the file is rewritten atomically, "
			       "but you may have caught a rare moment mid-write)\n",
"не удалось разобрать %s (попробуйте ещё раз -- файл перезаписывается атомарно, "
			       "но могли поймать редкий момент)\n"),
			       path);
			return 2;
		}
		print_summary(&st);
		return 0;
	}
}
