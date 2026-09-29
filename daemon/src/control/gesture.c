/* control/gesture.c -- see gesture.h. */
#include "gesture.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

static const char *const CMD_KEYS[M4_PLAYER_CMD_COUNT] = {
	"play", "pause", "playpause", "next", "previous", "stop",
};

static const char *const CMD_METHODS[M4_PLAYER_CMD_COUNT] = {
	"Play", "Pause", "PlayPause", "Next", "Previous", "Stop",
};

static const char *const ACTION_NAMES[M4_GESTURE_ACTION_COUNT] = {
	"none", "call-toggle", "call-on", "call-off", "passthrough", "passthrough-exact", "mic-toggle",
};

const char *m4_player_cmd_key(m4_player_cmd_t cmd)
{
	return (unsigned)cmd < M4_PLAYER_CMD_COUNT ? CMD_KEYS[cmd] : "?";
}

const char *m4_player_cmd_method(m4_player_cmd_t cmd)
{
	return (unsigned)cmd < M4_PLAYER_CMD_COUNT ? CMD_METHODS[cmd] : "?";
}

int m4_player_cmd_from_method(const char *method, m4_player_cmd_t *out)
{
	if (method == NULL)
		return -1;
	for (int i = 0; i < M4_PLAYER_CMD_COUNT; i++) {
		if (strcmp(method, CMD_METHODS[i]) == 0) {
			*out = (m4_player_cmd_t)i;
			return 0;
		}
	}
	return -1;
}

const char *m4_gesture_action_name(m4_gesture_action_t a)
{
	return (unsigned)a < M4_GESTURE_ACTION_COUNT ? ACTION_NAMES[a] : "?";
}

int m4_gesture_action_parse(const char *s, m4_gesture_action_t *out)
{
	if (s == NULL)
		return -1;
	for (int i = 0; i < M4_GESTURE_ACTION_COUNT; i++) {
		if (strcasecmp(s, ACTION_NAMES[i]) == 0) {
			*out = (m4_gesture_action_t)i;
			return 0;
		}
	}
	if (strcasecmp(s, "off") == 0 || strcasecmp(s, "ignore") == 0) {
		*out = M4_GESTURE_ACTION_NONE;
		return 0;
	}
	if (strcasecmp(s, "toggle-call") == 0) {
		*out = M4_GESTURE_ACTION_CALL_TOGGLE;
		return 0;
	}
	return -1;
}

const char *m4_player_status_policy_name(m4_player_status_policy_t p)
{
	switch (p) {
	case M4_PLAYER_STATUS_STOPPED: return "stopped";
	case M4_PLAYER_STATUS_PLAYING: return "playing";
	case M4_PLAYER_STATUS_PAUSED: return "paused";
	case M4_PLAYER_STATUS_FOLLOW: return "follow";
	}
	return "?";
}

int m4_player_status_policy_parse(const char *s, m4_player_status_policy_t *out)
{
	static const m4_player_status_policy_t all[] = {
		M4_PLAYER_STATUS_STOPPED, M4_PLAYER_STATUS_PLAYING,
		M4_PLAYER_STATUS_PAUSED, M4_PLAYER_STATUS_FOLLOW,
	};

	if (s == NULL)
		return -1;
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		if (strcasecmp(s, m4_player_status_policy_name(all[i])) == 0) {
			*out = all[i];
			return 0;
		}
	}
	return -1;
}

const char *m4_player_status_mpris(m4_player_status_policy_t concrete)
{
	switch (concrete) {
	case M4_PLAYER_STATUS_PLAYING: return "Playing";
	case M4_PLAYER_STATUS_PAUSED: return "Paused";
	case M4_PLAYER_STATUS_STOPPED:
	case M4_PLAYER_STATUS_FOLLOW:
	default: return "Stopped";
	}
}

m4_player_status_policy_t m4_player_status_initial(m4_player_status_policy_t policy)
{
	return policy == M4_PLAYER_STATUS_FOLLOW ? M4_PLAYER_STATUS_PLAYING : policy;
}

m4_player_status_policy_t m4_player_status_after(m4_player_status_policy_t policy,
						   m4_player_status_policy_t current,
						   m4_player_cmd_t cmd)
{
	if (policy != M4_PLAYER_STATUS_FOLLOW)
		return policy;

	switch (cmd) {
	case M4_PLAYER_CMD_PLAY: return M4_PLAYER_STATUS_PLAYING;
	case M4_PLAYER_CMD_PAUSE: return M4_PLAYER_STATUS_PAUSED;
	case M4_PLAYER_CMD_STOP: return M4_PLAYER_STATUS_STOPPED;
	case M4_PLAYER_CMD_PLAYPAUSE:
		return current == M4_PLAYER_STATUS_PLAYING ? M4_PLAYER_STATUS_PAUSED
							    : M4_PLAYER_STATUS_PLAYING;
	case M4_PLAYER_CMD_NEXT:
	case M4_PLAYER_CMD_PREVIOUS:
	case M4_PLAYER_CMD_COUNT:
	default:
		return current;
	}
}

const char *m4_player_resync_name(m4_player_resync_t r)
{
	switch (r) {
	case M4_PLAYER_RESYNC_ON: return "on";
	case M4_PLAYER_RESYNC_CONNECT: return "connect";
	case M4_PLAYER_RESYNC_OFF: return "off";
	}
	return "?";
}

int m4_player_resync_parse(const char *s, m4_player_resync_t *out)
{
	if (s == NULL || out == NULL)
		return -1;
	if (strcasecmp(s, "on") == 0 || strcasecmp(s, "yes") == 0 || strcasecmp(s, "true") == 0 ||
	    strcmp(s, "1") == 0) {
		*out = M4_PLAYER_RESYNC_ON;
		return 0;
	}
	if (strcasecmp(s, "connect") == 0) {
		*out = M4_PLAYER_RESYNC_CONNECT;
		return 0;
	}
	if (strcasecmp(s, "off") == 0 || strcasecmp(s, "no") == 0 || strcasecmp(s, "false") == 0 ||
	    strcmp(s, "0") == 0) {
		*out = M4_PLAYER_RESYNC_OFF;
		return 0;
	}
	return -1;
}

bool m4_player_resync_after_cmd(m4_player_resync_t mode, m4_player_status_policy_t policy, m4_player_cmd_t cmd)
{
	if (mode != M4_PLAYER_RESYNC_ON)
		return false;
	if (policy != M4_PLAYER_STATUS_PLAYING && policy != M4_PLAYER_STATUS_PAUSED)
		return false;
	return cmd == M4_PLAYER_CMD_PLAY || cmd == M4_PLAYER_CMD_PAUSE || cmd == M4_PLAYER_CMD_PLAYPAUSE ||
	       cmd == M4_PLAYER_CMD_STOP;
}

const char *m4_gesture_passthrough_method(m4_gesture_action_t action, m4_player_cmd_t cmd)
{
	if (action == M4_GESTURE_ACTION_PASSTHROUGH_EXACT)
		return m4_player_cmd_method(cmd);
	if (action != M4_GESTURE_ACTION_PASSTHROUGH)
		return NULL;
	if (cmd == M4_PLAYER_CMD_PLAY || cmd == M4_PLAYER_CMD_PAUSE)
		return "PlayPause";
	return m4_player_cmd_method(cmd);
}

/* --- HFP audio link dropped by M4 --------------------- */

const char *m4_sco_drop_policy_name(m4_sco_drop_policy_t p)
{
	switch (p) {
	case M4_SCO_DROP_RESTORE: return "restore";
	case M4_SCO_DROP_CALL_OFF: return "call-off";
	case M4_SCO_DROP_MIC_TOGGLE: return "mic-toggle";
	case M4_SCO_DROP_NONE: return "none";
	case M4_SCO_DROP_COUNT:
	default: return "?";
	}
}

int m4_sco_drop_policy_parse(const char *s, m4_sco_drop_policy_t *out)
{
	char buf[32];
	size_t i;

	if (s == NULL || out == NULL)
		return -1;
	for (i = 0; s[i] != '\0' && i < sizeof(buf) - 1; i++)
		buf[i] = s[i] == '_' ? '-' : (char)tolower((unsigned char)s[i]);
	if (s[i] != '\0')
		return -1;
	buf[i] = '\0';
	for (int p = 0; p < M4_SCO_DROP_COUNT; p++) {
		if (strcmp(buf, m4_sco_drop_policy_name((m4_sco_drop_policy_t)p)) == 0) {
			*out = (m4_sco_drop_policy_t)p;
			return 0;
		}
	}
	return -1;
}

const char *m4_sco_step_name(m4_sco_step_t step)
{
	switch (step) {
	case M4_SCO_STEP_NOTHING: return "nothing";
	case M4_SCO_STEP_EARCON_RESTORED: return "earcon-restored";
	case M4_SCO_STEP_MIC_TOGGLE: return "mic-toggle";
	case M4_SCO_STEP_CALL_OFF: return "call-off";
	case M4_SCO_STEP_CALL_OFF_FALLBACK: return "call-off-fallback";
	default: return "?";
	}
}

m4_sco_step_t m4_sco_drop_decide(m4_sco_drop_policy_t policy, m4_sco_event_t ev)
{
	switch (ev) {
	case M4_SCO_EVENT_DROP:
		return policy == M4_SCO_DROP_CALL_OFF ? M4_SCO_STEP_CALL_OFF : M4_SCO_STEP_NOTHING;
	case M4_SCO_EVENT_UP:
		if (policy == M4_SCO_DROP_RESTORE)
			return M4_SCO_STEP_EARCON_RESTORED;
		if (policy == M4_SCO_DROP_MIC_TOGGLE)
			return M4_SCO_STEP_MIC_TOGGLE;
		return M4_SCO_STEP_NOTHING;
	case M4_SCO_EVENT_FAIL:
		return policy == M4_SCO_DROP_NONE ? M4_SCO_STEP_NOTHING : M4_SCO_STEP_CALL_OFF_FALLBACK;
	default:
		return M4_SCO_STEP_NOTHING;
	}
}
