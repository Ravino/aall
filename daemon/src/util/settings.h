/* util/settings.h -- ~/.config/momentum4/momentum4.conf .
 *
 * Format: one `key = value` per line; blank lines and lines starting with
 * '#' or ';' are ignored; a ' #' after the value starts a trailing comment;
 * keys are case-insensitive and '_' == '-'; values may be "quoted".
 * Unknown keys / invalid values are WARNINGS (logged, counted, that one key
 * keeps its default) -- never fatal: a typo in the config must not keep the
 * daemon from starting (Restart=always would just loop).
 *
 *   mode                = observe | manual | auto        (default manual;
 * read at daemon start -- survives
 *                         restarts/reboots; `momentum4 mode X` changes it
 *                         only until the next restart)
 *   player              = on | off                        (default on:
 *                         register our MPRIS player in BlueZ)
 *   player-status       = stopped | playing | paused | follow (default stopped)
 *   player-resync       = on | connect | off                (default on:
 * re-send the status to M4)
 *   gesture-play        = <action>   (default: same as gesture-playpause)
 *   gesture-pause       = <action>   (default: same as gesture-playpause)
 *   gesture-playpause   = <action>   (default passthrough)
 *   gesture-next        = <action>   (default call-toggle)
 *   gesture-previous    = <action>   (default none)
 *   gesture-stop        = <action>   (default none)
 *   gesture-cooldown-ms = 0..60000   (default 1500) -- after a call-on/off/
 *                         toggle finishes, further call actions are ignored
 *                         this long (gestures queued while the profile switch
 *                         was blocking must not toggle straight back)
 *   earcon              = on | off   (default on) -- call-on/call-off .wav via pw-play
 *   earcon-dir          = <dir with call-on.wav / call-off.wav / call-error.wav>
 * earcon-delay-on-ms = 0.30000 (default 400):
 *                         the M4 combine-sink participant is torn down and
 *                         recreated on every profile switch, and (for a
 *                         switch INTO A2DP) the aptX Adaptive helper only
 *                         starts producing frames some tens/hundreds of ms
 *                         after the switch is confirmed -- playing call-on
 *                         immediately mostly plays into a stream that is not
 *                         there yet. This delays the call-on earcon (never
 *                         the switch itself, which is never delayed) by a
 *                         fixed amount; see daemon.c's daemon_schedule_earcon()
 *                         doc comment for why "wait for the M4 node to reach
 *                         RUNNING" was NOT implemented instead.
 *   earcon-delay-off-ms = 0..30000  (default 500) -- same, for call-off (into HFP)
 * hfp-chup = <call-action> (default call-off,
 * patch 0006: AT+CHUP received by
 *                         backend-native.c with no ModemManager -- patch 0006
 *                         answers OK and forwards it here instead of the
 *                         usual +CME ERROR)
 *   hfp-ata             = <call-action>   (default none; AT answer-call)
 *   hfp-bldn            = <call-action>   (default none; AT+BLDN redial-last)
 *   hfp-bvra            = <call-action>   (default none; AT+BVRA voice-recognition
 *                         toggle -- only ever arrives if backend-native.c's
 *                         AG features advertise VOICE_RECOG, which they
 * currently do not.2)
 *   hfp-sco-drop        = restore | call-off | mic-toggle | none   (default restore;
 * M4 closes the HFP audio link on a
 *                         double tap; patch 0008 reconnects it and reports
 *                         SCODROP/SCOUP/SCOFAIL -- see control/gesture.h)
 *   mic-on-enter        = on | off | remember   (default on; `[call]
 *                         mic-on-enter`, common/m4-conf-keys.def -- on:
 * always unmuted at HFP entry (Bug 2,
 *                         unchanged); off: always muted; remember: restored
 *                         from the mic state at the LAST HFP exit, see
 *                         core/state.h's m4_mic_state_read()/_write())
 *   mic-off-chain-delay-ms = 0..30000  (default 300) -- when mic-on-enter
 *                         leaves the mic muted, mic-off plays this long
 *                         after call-on actually played (not from the
 *                         switch itself) so the two sounds do not overlap
 *   <action> = none | call-toggle | call-on | call-off | passthrough | passthrough-exact | mic-toggle
 *              (passthrough/passthrough-exact are accepted by the parser for
 *              hfp-* keys too, for symmetry, but have no session-MPRIS
 *              analog for a telephony AT command -- momentum4d logs a
 *              warning and treats them as none, see daemon.c's
 *              m4_daemon_hfp_event())
 */
#ifndef MOMENTUM4_UTIL_SETTINGS_H
#define MOMENTUM4_UTIL_SETTINGS_H

#include <stdbool.h>

#include "../control/gesture.h"
#include "../core/state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M4_SETTINGS_DEFAULT_COOLDOWN_MS 1500
#define M4_SETTINGS_MAX_COOLDOWN_MS 60000

#define M4_SETTINGS_DEFAULT_EARCON_DELAY_ON_MS 400 /* see registry defaults above */
#define M4_SETTINGS_DEFAULT_EARCON_DELAY_OFF_MS 500
#define M4_SETTINGS_MAX_EARCON_DELAY_MS 30000

struct m4_settings {
	m4_daemon_mode_t mode;
	bool player_enabled;
	m4_player_status_policy_t player_status;
	/* player status after reconnect: `[control]
	 * player-resync` (control/gesture.h m4_player_resync_t) */
	m4_player_resync_t player_resync;
	m4_gesture_action_t gesture[M4_PLAYER_CMD_COUNT];
	int gesture_cooldown_ms;
	bool earcon;
	char earcon_dir[256]; /* "" -> built-in search list (util/earcon.h) */
	int earcon_delay_on_ms;  /* daemon.c's daemon_schedule_earcon(); 0 -> play immediately */
	int earcon_delay_off_ms;

	/* HFP AT call-control command patch 0006 forwards (see
	 * ipc/hfp_events.h's m4_hfp_verb_t) when there is no ModemManager to
	 * answer it "for real". Same gesture_cooldown_ms above applies. */
	m4_gesture_action_t hfp_chup;
	m4_gesture_action_t hfp_ata;
	m4_gesture_action_t hfp_bldn;
	m4_gesture_action_t hfp_bvra;
	/* what to add when M4 drops the HFP audio link */
	m4_sco_drop_policy_t hfp_sco_drop;

	/* mic-on-enter: `[call] mic-on-enter`/`[earcon]
	 * mic-off-chain-delay-ms` (common/m4-conf-keys.def) -- physically live
	 * in different modern-registry sections, folded into this one flat
	 * struct the same way hfp_chup/hfp_sco_drop above (both `[control]`)
	 * already are, so momentum4d test fixtures can inject them through the
	 * SAME settings_text mechanism as every other knob here. */
	m4_mic_on_enter_t mic_on_enter;
	int mic_off_chain_delay_ms;

	int n_warnings;
	char first_warning[200];
};

void m4_settings_defaults(struct m4_settings *s);

/* Parses `text` ON TOP of whatever `s` already holds (call
 * m4_settings_defaults() first). Every warning is logged (WARN) and
 * counted in s->n_warnings. `origin` is only used in log lines. */
void m4_settings_parse(struct m4_settings *s, const char *text, const char *origin);

/* m4_settings_defaults() + parse the file at `path`.
 * Returns 0 if the file was read, 1 if it does not exist (defaults kept,
 * not an error), -1 on a read error (defaults kept, logged). `s` is always
 * fully valid afterwards. */
int m4_settings_load(struct m4_settings *s, const char *path);

/* Same as m4_settings_load(), plus one extra WARN-level log line (and, if it
 * is the only warning, `s->first_warning`) when `path` exists and is still
 * the OLD flat (no [section]) format -- the old flat
 * momentum4.conf is still read, with a warning. Used by
 * cli/daemon_cmd.c at normal daemon startup; m4_settings_load() itself is
 * unchanged (tests call it directly and do not want this extra warning). */
int m4_settings_load_with_migration_hint(struct m4_settings *s, const char *path);

/* true for 1/yes/on/true, false for 0/no/off/false; -1 if neither. */
int m4_settings_parse_bool(const char *v);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_UTIL_SETTINGS_H */
