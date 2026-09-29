/* cli/stats.h -- `momentum4 stats`: short screen-reader-friendly summary of
 * the aptX Adaptive bridge's live metrics (metrics/stats.h's own m4_stats,
 * written by the SPA plugin to `[stats] path`).
 */
#ifndef MOMENTUM4_CLI_STATS_H
#define MOMENTUM4_CLI_STATS_H

#ifdef __cplusplus
extern "C" {
#endif

int m4_cli_stats_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CLI_STATS_H */
