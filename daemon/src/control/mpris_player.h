/* control/mpris_player.h -- momentum4d's own MPRIS player, registered in
 * BlueZ via org.bluez.Media1.RegisterPlayer.
 *
 * Why: with no registered player BlueZ turns M4's AVRCP passthrough (tap,
 * swipes, Smart Pause) into uinput key events ("MOMENTUM 4 (AVRCP)") that
 * go to X/mate-settings-daemon; with a player, BlueZ instead CALLS our
 * org.mpris.MediaPlayer2.Player.Play/Pause/Stop/Next/Previous over the
 * system bus (profiles/audio/avrcp.c handle_passthrough() ->
 * media.c media_player_send(); only on press, never on release; Play/
 * Pause/Next/Previous only if we advertised CanPlay/CanPause/CanGoNext/
 * CanGoPrevious AND CanControl, Stop only if CanControl).
 *
 * Which player BlueZ 5.66 addresses (confirmed by reading avrcp.c):
 * the FIRST one in its per-adapter list -- target_init()
 * takes g_slist_nth_data(server->players, 0) for a new AVRCP session,
 * avrcp_register_player() appends and only assigns itself to sessions that
 * have NO player yet, and SetAddressedPlayer from the CT does not change
 * target->player. So "first registered wins", not "last". PipeWire's
 * bluez5.dummy-avrcp-player is off by default and not enabled on this
 * machine; if it were enabled and registered before us, M4 would address it
 * (it has no CanControl -> every passthrough falls back to uinput) and we
 * would get nothing. BlueZ does not expose its player list over D-Bus; the
 * live test checks bluetoothd's own "Player registered:
 * sender=... path=..." journal lines instead (none this boot before us).
 *
 * Lifecycle: tracks the org.bluez name owner (NameOwnerChanged) and the
 * adapter's org.bluez.Media1 interface (ObjectManager InterfacesAdded/
 * Removed) and re-registers after a bluetoothd restart / adapter re-add;
 * a failed RegisterPlayer is retried every retry_ms. BlueZ drops the player
 * by itself when our bus connection goes away (disconnect watch), so a
 * killed daemon leaves nothing behind.
 *
 * Commands are acknowledged immediately (empty method return) and handed to
 * `cb` from an sd-event defer source, never from inside the D-Bus handler.
 */
#ifndef MOMENTUM4_CONTROL_MPRIS_PLAYER_H
#define MOMENTUM4_CONTROL_MPRIS_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M4_MPRIS_PLAYER_PATH "/org/momentum4/player"
#define M4_MPRIS_PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define M4_MPRIS_ROOT_IFACE "org.mpris.MediaPlayer2"
#define M4_BLUEZ_SERVICE "org.bluez"
#define M4_BLUEZ_MEDIA_IFACE "org.bluez.Media1"

struct m4_player_event {
	m4_player_cmd_t cmd;
	char sender[64];          /* D-Bus unique name of the caller */
	uint64_t realtime_usec;   /* CLOCK_REALTIME when the call was received */
	double monotonic_s;       /* CLOCK_MONOTONIC seconds, same moment */
};

typedef void (*m4_mpris_player_cmd_cb)(void *user_data, const struct m4_player_event *ev);

struct m4_mpris_player_config {
	sd_bus *bus;               /* already started + attached to `event`; ref'd */
	const char *bluez_service; /* NULL -> "org.bluez" */
	const char *adapter_path;  /* NULL -> "/org/bluez/hci0" */
	const char *object_path;   /* NULL -> M4_MPRIS_PLAYER_PATH */
	const char *identity;      /* NULL -> "momentum4" */
	m4_player_status_policy_t initial_status; /* concrete: stopped/playing/paused */
	int retry_ms;              /* <= 0 -> 10000 */
	int reappear_delay_ms;     /* <= 0 -> 1000 (org.bluez or Media1 (re)appeared) */
	bool accept_any_sender;    /* false (production): calls not coming from
				    * the current org.bluez owner are rejected */
	/* Status re-send ("resync", control/gesture.h m4_player_resync_t,
	 * player status after reconnect). */
	const char *device_path;   /* M4's Device1 object path; NULL/"" -> any
				    * device under adapter_path */
	bool auto_resync;          /* resync by itself on: successful
				    * RegisterPlayer; Device1/MediaControl1
				    * Connected -> true; InterfacesAdded
				    * MediaPlayer1 under the device;
				    * MediaTransport1 State -> "active" */
	int resync_delay_ms;       /* <= 0 -> 800: trigger -> Stopped (debounce:
				    * every new trigger restarts it) */
	int resync_gap_ms;         /* <= 0 -> 500: Stopped -> real status */
	m4_mpris_player_cmd_cb cb;
	void *user_data;
};

struct m4_mpris_player;

/* NULL on failure to export the object (logged). Registration itself is
 * asynchronous and never fails this call. */
struct m4_mpris_player *m4_mpris_player_new(sd_event *event, const struct m4_mpris_player_config *cfg);

/* Best-effort UnregisterPlayer (fire-and-forget + flush), then frees. NULL-safe. */
void m4_mpris_player_free(struct m4_mpris_player *p);

/* Changes the reported PlaybackStatus (concrete value; FOLLOW is treated as
 * STOPPED) and emits PropertiesChanged if it differs. NULL-safe. */
void m4_mpris_player_set_status(struct m4_mpris_player *p, m4_player_status_policy_t concrete);

/* Re-sends the CURRENT status to M4 as a real change: after resync_delay_ms
 * PlaybackStatus reads "Stopped" (PropertiesChanged), after resync_gap_ms
 * more the real value again (PropertiesChanged). BlueZ turns each into an
 * AVRCP EVENT_PLAYBACK_STATUS_CHANGED for M4 (media.c set_status() ignores
 * an unchanged value -- hence the detour through Stopped). Debounced: a
 * trigger during the delay restarts it, one during the Stopped phase
 * schedules exactly one more resync afterwards. No-op (logged) if not
 * registered or the real status is Stopped. `reason` goes to the journal.
 * NULL-safe. */
void m4_mpris_player_resync(struct m4_mpris_player *p, const char *reason);
/* Enables/disables the automatic triggers (cfg.auto_resync). NULL-safe. */
void m4_mpris_player_set_auto_resync(struct m4_mpris_player *p, bool on);
int m4_mpris_player_resync_count(const struct m4_mpris_player *p); /* completed Stopped->real round trips */

bool m4_mpris_player_is_registered(const struct m4_mpris_player *p);
int m4_mpris_player_register_count(const struct m4_mpris_player *p); /* successful registrations */
int m4_mpris_player_rejected_count(const struct m4_mpris_player *p); /* calls from foreign senders */

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CONTROL_MPRIS_PLAYER_H */
