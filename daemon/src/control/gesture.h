/* control/gesture.h -- M4 touch gestures / wear sensor -> momentum4 actions.
 *
 * What arrives: M4 sends AVRCP passthrough PLAY/PAUSE/STOP/FORWARD/BACKWARD;
 * BlueZ 5.66 turns each press (never the release) into ONE D-Bus method
 * call on the registered MPRIS player (profiles/audio/avrcp.c
 * handle_passthrough() -> media.c media_player_send()): Play, Pause, Stop,
 * Next, Previous. BlueZ itself never sends PlayPause; it is accepted anyway
 * (standard MPRIS method, useful for manual `busctl call` tests). Which
 * physical gesture produces which command is exactly what the live test
 * against a real M4 is for -- by the M4 manual/ADK: tap ->
 * Play (while we report Stopped; Play/Pause alternately if we report
 * Playing), swipe forward -> Next, swipe back -> Previous, take off/put on ->
 * Pause/Play only while we report Playing (Smart Pause, likely).
 *
 * Pure: no I/O, no D-Bus -- parse/format/lookup only (tested in
 * tests/test_settings.c).
 */
#ifndef MOMENTUM4_CONTROL_GESTURE_H
#define MOMENTUM4_CONTROL_GESTURE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	M4_PLAYER_CMD_PLAY = 0,
	M4_PLAYER_CMD_PAUSE,
	M4_PLAYER_CMD_PLAYPAUSE,
	M4_PLAYER_CMD_NEXT,
	M4_PLAYER_CMD_PREVIOUS,
	M4_PLAYER_CMD_STOP,
	M4_PLAYER_CMD_COUNT,
} m4_player_cmd_t;

typedef enum {
	M4_GESTURE_ACTION_NONE = 0,      /* only logged */
	M4_GESTURE_ACTION_CALL_TOGGLE,   /* momentum4 call on/off, by current state */
	M4_GESTURE_ACTION_CALL_ON,
	M4_GESTURE_ACTION_CALL_OFF,
	M4_GESTURE_ACTION_PASSTHROUGH,   /* forward to the session's MPRIS player;
					  * Play/Pause are forwarded as PlayPause
					  * (M4 cannot know the real player's state --
					  * our reported status is synthetic) */
	M4_GESTURE_ACTION_PASSTHROUGH_EXACT, /* forward the very same method */
	/* "tap = mic" action: toggles
	 * the M4 mic Route's mute (core/graph_ops.h's set_mic_volume(), the
	 * same PipeWire object the earlier mic-volume fix already
	 * targets) -- meaningful only while a call is active (headset-head-
	 * unit[-cvsd], where the mic Route actually exists); daemon.c's
	 * handler no-ops (+ call-error earcon) otherwise. Deliberately its own
	 * action rather than reusing CALL_ON/OFF's machinery: this never
	 * touches Profile, only the mic Route, and is not subject to
	 * callfsm's profile-confirm timeout. */
	M4_GESTURE_ACTION_MIC_TOGGLE,
	M4_GESTURE_ACTION_COUNT,
} m4_gesture_action_t;

/* What PlaybackStatus our BlueZ player reports to M4 (AVRCP
 * EVENT_PLAYBACK_STATUS_CHANGED / GetPlayStatus):
 *   STOPPED -- same as today without any player (BlueZ answers STOPPED);
 *              Smart Pause (wear sensor) most likely stays silent, tap
 * always sends Play ;
 *   PLAYING / PAUSED -- fixed value;
 *   FOLLOW  -- behave like a real player: starts Playing, Play -> Playing,
 *              Pause -> Paused, PlayPause flips, Stop -> Stopped. Enables
 *              Smart Pause (take off -> Pause, put on -> Play). */
typedef enum {
	M4_PLAYER_STATUS_STOPPED = 0,
	M4_PLAYER_STATUS_PLAYING,
	M4_PLAYER_STATUS_PAUSED,
	M4_PLAYER_STATUS_FOLLOW,
} m4_player_status_policy_t;

/* "play", "pause", "playpause", "next", "previous", "stop" */
const char *m4_player_cmd_key(m4_player_cmd_t cmd);
/* MPRIS method names: "Play", "Pause", "PlayPause", "Next", "Previous", "Stop" */
const char *m4_player_cmd_method(m4_player_cmd_t cmd);
/* Parses an MPRIS method name (exact case). 0 on success, -1 otherwise. */
int m4_player_cmd_from_method(const char *method, m4_player_cmd_t *out);

/* "none", "call-toggle", "call-on", "call-off", "passthrough", "passthrough-exact", "mic-toggle" */
const char *m4_gesture_action_name(m4_gesture_action_t a);
/* Case-insensitive; also accepts "off"/"ignore" for none, "toggle-call"
 * for call-toggle. 0 on success, -1 otherwise. */
int m4_gesture_action_parse(const char *s, m4_gesture_action_t *out);

/* "stopped", "playing", "paused", "follow" */
const char *m4_player_status_policy_name(m4_player_status_policy_t p);
int m4_player_status_policy_parse(const char *s, m4_player_status_policy_t *out);

/* MPRIS PlaybackStatus string for a concrete (non-FOLLOW) status value:
 * "Stopped"/"Playing"/"Paused". */
const char *m4_player_status_mpris(m4_player_status_policy_t concrete);

/* Status after `cmd` under `policy`, given `current` (a concrete status).
 * Non-FOLLOW policies always return the policy's own fixed value. */
m4_player_status_policy_t m4_player_status_after(m4_player_status_policy_t policy,
						   m4_player_status_policy_t current,
						   m4_player_cmd_t cmd);

/* Initial concrete status for a policy (FOLLOW starts PLAYING). */
m4_player_status_policy_t m4_player_status_initial(m4_player_status_policy_t policy);

/* --- PlaybackStatus re-send ("resync" after reconnect) ------------------
 * M4 keeps its OWN idea of "is the source playing" and sends Next/Previous
 * (swipes) only while it believes "playing" (observed live: status
 * Stopped -> swipes silent; also swipes silent right
 * after a tap that M4 sent as Pause, restored instantly by Stopped ->
 * Playing). M4 flips that belief on its own Play/Pause (taps alternate
 * Pause/Play although we report a fixed Playing), and a status that did
 * not CHANGE never reaches it: BlueZ media.c set_status() drops a
 * PropertiesChanged with the same value, and a freshly registered player
 * never sends AVRCP CHANGED to an existing session. The only way to push
 * our status to M4 again is a real change: Stopped, then the real value
 * (two AVRCP EVENT_PLAYBACK_STATUS_CHANGED notifications).
 *   ON      (default) resync on connect/registration/A2DP active (CONNECT)
 *           AND after every Play/Pause/PlayPause/Stop M4 sends while the
 *           status policy is a FIXED playing/paused (M4 just flipped its
 *           belief away from ours -- flip it back);
 *   CONNECT only: our registration in BlueZ (daemon start, bluetoothd
 *           restart, adapter re-add), Device1/MediaControl1 Connected ->
 *           true, a new MediaPlayer1 under the device, MediaTransport1
 *           State -> active (reconnect, return from HFP);
 *   OFF     never (behaviour before this change). */
typedef enum {
	M4_PLAYER_RESYNC_ON = 0,
	M4_PLAYER_RESYNC_CONNECT,
	M4_PLAYER_RESYNC_OFF,
} m4_player_resync_t;

/* "on", "connect", "off" */
const char *m4_player_resync_name(m4_player_resync_t r);
/* Case-insensitive; also "yes"/"true"/"1" -> on, "no"/"false"/"0" -> off. 0 or -1. */
int m4_player_resync_parse(const char *s, m4_player_resync_t *out);
/* Pure decision: re-send the status after M4 sent `cmd`? True only for
 * mode ON, a FIXED playing/paused policy, and Play/Pause/PlayPause/Stop. */
bool m4_player_resync_after_cmd(m4_player_resync_t mode, m4_player_status_policy_t policy, m4_player_cmd_t cmd);

/* Method to call on the session's MPRIS player for a passthrough action
 * (NULL if `action` is not a passthrough). */
const char *m4_gesture_passthrough_method(m4_gesture_action_t action, m4_player_cmd_t cmd);

/* --- HFP audio link dropped by M4 ---------
 * In HFP, a double tap on the M4 touch panel makes the headset release the
 * eSCO link itself (no AT command). Patch 0008 in our libspa-bluez5 restores
 * the link in place and reports over hfp-events.sock: SCODROP (link lost),
 * then exactly one of SCOUP (restored) / SCOFAIL (gave up), unless the
 * transport was released in between (profile switch, disconnect).
 * momentum4.conf `hfp-sco-drop` decides what the daemon adds on top:
 *   restore    (default) earcon when restored; fallback to A2DP on SCOFAIL
 *   call-off   double tap = leave call mode (A2DP) right away
 *   mic-toggle restored + toggle the mic (the double tap becomes a mic gesture);
 *              fallback to A2DP on SCOFAIL
 *   none       log only (no earcon, no fallback) */
typedef enum {
	M4_SCO_EVENT_DROP = 0,
	M4_SCO_EVENT_UP,
	M4_SCO_EVENT_FAIL,
} m4_sco_event_t;

typedef enum {
	M4_SCO_DROP_RESTORE = 0,
	M4_SCO_DROP_CALL_OFF,
	M4_SCO_DROP_MIC_TOGGLE,
	M4_SCO_DROP_NONE,
	M4_SCO_DROP_COUNT,
} m4_sco_drop_policy_t;

typedef enum {
	M4_SCO_STEP_NOTHING = 0,
	M4_SCO_STEP_EARCON_RESTORED, /* play call-on: "call audio is back" */
	M4_SCO_STEP_MIC_TOGGLE,
	M4_SCO_STEP_CALL_OFF,        /* policy call-off on SCODROP */
	M4_SCO_STEP_CALL_OFF_FALLBACK, /* restore failed: back to A2DP so sound returns */
} m4_sco_step_t;

const char *m4_sco_drop_policy_name(m4_sco_drop_policy_t p);
/* "restore" | "call-off" | "mic-toggle" | "none" (case-insensitive, '_' == '-'); 0 or -1 */
int m4_sco_drop_policy_parse(const char *s, m4_sco_drop_policy_t *out);
const char *m4_sco_step_name(m4_sco_step_t step);
/* Pure decision table (tests/test_sco_drop.c). */
m4_sco_step_t m4_sco_drop_decide(m4_sco_drop_policy_t policy, m4_sco_event_t ev);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_CONTROL_GESTURE_H */
