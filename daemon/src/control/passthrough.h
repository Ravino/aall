/* control/passthrough.h -- forward a gesture to the session's active MPRIS
 * player .
 *
 * Abstract ops so daemon tests use a fake; the real implementation talks to
 * the user's SESSION bus (where LibreWolf/Firefox, mpv, etc. publish
 * org.mpris.MediaPlayer2.*), never to the system bus.
 *
 * Target choice (same idea as playerctl): among bus names starting with
 * "org.mpris.MediaPlayer2.", the first (name-sorted) one whose
 * PlaybackStatus is "Playing"; else the first "Paused"; else the first one
 * at all. No player -> nothing happens (logged).
 */
#ifndef MOMENTUM4_CONTROL_PASSTHROUGH_H
#define MOMENTUM4_CONTROL_PASSTHROUGH_H

#include <stddef.h>

#include <systemd/sd-bus.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M4_MPRIS_NAME_PREFIX "org.mpris.MediaPlayer2."
#define M4_PASSTHROUGH_MAX_CANDIDATES 16

struct m4_passthrough_ops {
	void *ctx;
	/* Calls `method` ("PlayPause", "Next", ...) on the chosen player.
	 * 0: sent (target name in `target`); 1: no player on the bus;
	 * -1: error (reason in `target`). Never blocks longer than a few
	 * short (<= 300 ms) D-Bus round-trips. */
	int (*forward)(void *ctx, const char *method, char *target, size_t target_cap);
};

struct m4_mpris_candidate {
	char name[128];
	char status[16]; /* "Playing"/"Paused"/"Stopped"/"" (unknown) */
};

/* Pure selection rule (see top comment). Returns index into `c` or -1. */
int m4_passthrough_pick(const struct m4_mpris_candidate *c, size_t n);

/* Real implementation. `bus` NULL -> the user's session bus is opened
 * lazily on first use (and reopened if it dies); a non-NULL bus (tests: a
 * private dbus-daemon) is used as-is and ref'd. */
struct m4_passthrough_sdbus;
struct m4_passthrough_sdbus *m4_passthrough_sdbus_new(sd_bus *bus);
void m4_passthrough_sdbus_free(struct m4_passthrough_sdbus *pt);
struct m4_passthrough_ops m4_passthrough_sdbus_ops(struct m4_passthrough_sdbus *pt);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CONTROL_PASSTHROUGH_H */
