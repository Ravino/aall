/* tests/test_settings.c -- util/settings.c (momentum4.conf), control/
 * gesture.c (names, status policy) and control/passthrough.c's pure target
 * choice . No I/O except one tmp file for
 * m4_settings_load(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "control/gesture.h"
#include "control/passthrough.h"
#include "util/config.h"
#include "util/settings.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                          \
	do {                                                                      \
		checks++;                                                         \
		if (!(cond)) {                                                    \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++;                                               \
		}                                                                 \
	} while (0)

static void test_defaults(void)
{
	struct m4_settings s;

	m4_settings_defaults(&s);
	CHECK(s.mode == M4_DAEMON_MODE_MANUAL, "default mode manual ");
	CHECK(s.player_enabled, "player on by default");
	CHECK(s.player_status == M4_PLAYER_STATUS_STOPPED, "player-status stopped by default");
	CHECK(s.player_resync == M4_PLAYER_RESYNC_ON, "player-resync on by default ");
	CHECK(s.gesture[M4_PLAYER_CMD_NEXT] == M4_GESTURE_ACTION_CALL_TOGGLE, "next -> call-toggle");
	CHECK(s.gesture[M4_PLAYER_CMD_PREVIOUS] == M4_GESTURE_ACTION_NONE, "previous -> none");
	CHECK(s.gesture[M4_PLAYER_CMD_PLAYPAUSE] == M4_GESTURE_ACTION_PASSTHROUGH, "playpause -> passthrough");
	CHECK(s.gesture[M4_PLAYER_CMD_PLAY] == M4_GESTURE_ACTION_PASSTHROUGH, "play inherits");
	CHECK(s.gesture[M4_PLAYER_CMD_PAUSE] == M4_GESTURE_ACTION_PASSTHROUGH, "pause inherits");
	CHECK(s.gesture[M4_PLAYER_CMD_STOP] == M4_GESTURE_ACTION_NONE, "stop -> none");
	CHECK(s.gesture_cooldown_ms == 300, "cooldown 300");
	CHECK(s.earcon, "earcon on");
	CHECK(s.earcon_delay_on_ms == 400, "earcon-delay-on-ms default 400 ");
	CHECK(s.earcon_delay_off_ms == 500, "earcon-delay-off-ms default 500 ");
	CHECK(s.hfp_chup == M4_GESTURE_ACTION_CALL_OFF, "hfp-chup default call-off ");
	CHECK(s.hfp_ata == M4_GESTURE_ACTION_NONE, "hfp-ata default none");
	CHECK(s.hfp_bldn == M4_GESTURE_ACTION_NONE, "hfp-bldn default none");
	CHECK(s.hfp_bvra == M4_GESTURE_ACTION_NONE, "hfp-bvra default none");
	CHECK(s.mic_on_enter == M4_MIC_ON_ENTER_ON, "mic-on-enter default on");
	CHECK(s.mic_off_chain_delay_ms == 300, "mic-off-chain-delay-ms default 300");
	CHECK(s.n_warnings == 0, "no warnings");
}

/* mic-on-enter: `[call] mic-on-enter` / `[earcon]
 * mic-off-chain-delay-ms` -- parse, case/alias handling, bad-value keeps
 * default (same mechanism test_warnings_keep_defaults() covers generically). */
static void test_mic_on_enter(void)
{
	struct m4_settings s;

	m4_settings_defaults(&s);
	m4_settings_parse(&s, "mic-on-enter = OFF\nmic-off-chain-delay-ms = 250\n", "t");
	CHECK(s.n_warnings == 0, "mic-on-enter/chain-delay parse without warnings");
	CHECK(s.mic_on_enter == M4_MIC_ON_ENTER_OFF, "mic-on-enter overridden to off (case-insensitive)");
	CHECK(s.mic_off_chain_delay_ms == 250, "mic-off-chain-delay-ms overridden to 250");

	m4_settings_defaults(&s);
	m4_settings_parse(&s, "mic_on_enter = remember\n", "t"); /* underscore == dash */
	CHECK(s.n_warnings == 0, "underscore key form");
	CHECK(s.mic_on_enter == M4_MIC_ON_ENTER_REMEMBER, "mic-on-enter overridden to remember");

	m4_settings_defaults(&s);
	m4_settings_parse(&s, "mic-on-enter = sometimes\nmic-off-chain-delay-ms = -1\n", "t");
	CHECK(s.n_warnings == 2, "2 warnings");
	CHECK(s.mic_on_enter == M4_MIC_ON_ENTER_ON, "bad mic-on-enter keeps default on");
	CHECK(s.mic_off_chain_delay_ms == 300, "bad mic-off-chain-delay-ms keeps default 300");

	CHECK(strcmp(m4_mic_on_enter_name(M4_MIC_ON_ENTER_ON), "on") == 0, "name on");
	CHECK(strcmp(m4_mic_on_enter_name(M4_MIC_ON_ENTER_OFF), "off") == 0, "name off");
	CHECK(strcmp(m4_mic_on_enter_name(M4_MIC_ON_ENTER_REMEMBER), "remember") == 0, "name remember");
	{
		m4_mic_on_enter_t v;

		CHECK(m4_mic_on_enter_parse("On", &v) == 0 && v == M4_MIC_ON_ENTER_ON, "parse On");
		CHECK(m4_mic_on_enter_parse("bogus", &v) < 0, "parse bogus fails");
	}
}

static void test_parse_example(void)
{
	struct m4_settings s;
	const char *text =
		"# momentum4.conf\n"
		"  mode = auto   # trailing comment\n"
		"\n"
		"; ini-style comment\n"
		"gesture-next = call-toggle\n"
		"gesture-previous = none\n"
		"GESTURE_PLAYPAUSE = \"passthrough\"\n"
		"player-status = follow\n"
		"gesture-cooldown-ms=2500\n"
		"earcon = off\n"
		"earcon-dir = /tmp/snd#1\n";

	m4_settings_defaults(&s);
	m4_settings_parse(&s, text, "test");
	CHECK(s.n_warnings == 0, "example parses without warnings");
	CHECK(s.mode == M4_DAEMON_MODE_AUTO, "mode auto");
	CHECK(s.player_status == M4_PLAYER_STATUS_FOLLOW, "follow");
	CHECK(s.gesture_cooldown_ms == 2500, "cooldown 2500");
	CHECK(!s.earcon, "earcon off");
	CHECK(strcmp(s.earcon_dir, "/tmp/snd#1") == 0, "'#' without preceding space stays in value");
	CHECK(s.gesture[M4_PLAYER_CMD_PLAYPAUSE] == M4_GESTURE_ACTION_PASSTHROUGH, "quoted + upper/underscore key");
}

static void test_hfp_and_earcon_delay(void)
{
	struct m4_settings s;

	m4_settings_defaults(&s);
	m4_settings_parse(&s,
			  "hfp-chup = call-toggle\n"
			  "hfp-ata = call-on\n"
			  "HFP_BLDN = call-off\n" /* case-insensitive, '_' == '-' */
			  "hfp-bvra = none\n"
			  "earcon-delay-on-ms = 0\n"
			  "earcon-delay-off-ms = 1200\n",
			  "t");
	CHECK(s.n_warnings == 0, "hfp-*/earcon-delay-* parse without warnings");
	CHECK(s.hfp_chup == M4_GESTURE_ACTION_CALL_TOGGLE, "hfp-chup overridden");
	CHECK(s.hfp_ata == M4_GESTURE_ACTION_CALL_ON, "hfp-ata overridden");
	CHECK(s.hfp_bldn == M4_GESTURE_ACTION_CALL_OFF, "hfp-bldn overridden (underscore key)");
	CHECK(s.hfp_bvra == M4_GESTURE_ACTION_NONE, "hfp-bvra explicit none");
	CHECK(s.earcon_delay_on_ms == 0, "earcon-delay-on-ms overridden to 0");
	CHECK(s.earcon_delay_off_ms == 1200, "earcon-delay-off-ms overridden to 1200");

	/* bad values keep the default, exactly like every other key
	 * (test_warnings_keep_defaults() below covers the general mechanism;
	 * this is the hfp-x/earcon-delay-x-specific instance of it) */
	m4_settings_defaults(&s);
	m4_settings_parse(&s,
			  "hfp-chup = not-a-real-action\n"
			  "earcon-delay-on-ms = -1\n"
			  "earcon-delay-off-ms = 99999999\n",
			  "t");
	CHECK(s.n_warnings == 3, "3 warnings");
	CHECK(s.hfp_chup == M4_GESTURE_ACTION_CALL_OFF, "bad hfp-chup keeps default call-off");
	CHECK(s.earcon_delay_on_ms == 400, "negative earcon-delay-on-ms keeps default 400");
	CHECK(s.earcon_delay_off_ms == 500, "too-large earcon-delay-off-ms keeps default 500");
}

static void test_inheritance(void)
{
	struct m4_settings s;

	m4_settings_defaults(&s);
	m4_settings_parse(&s, "gesture-playpause = call-toggle\ngesture-pause = none\n", "t");
	CHECK(s.gesture[M4_PLAYER_CMD_PLAY] == M4_GESTURE_ACTION_CALL_TOGGLE, "play inherits playpause");
	CHECK(s.gesture[M4_PLAYER_CMD_PAUSE] == M4_GESTURE_ACTION_NONE, "explicit pause wins");

	/* order independent: explicit play before playpause */
	m4_settings_defaults(&s);
	m4_settings_parse(&s, "gesture-play = call-on\ngesture-playpause = none\n", "t");
	CHECK(s.gesture[M4_PLAYER_CMD_PLAY] == M4_GESTURE_ACTION_CALL_ON, "explicit play kept");
	CHECK(s.gesture[M4_PLAYER_CMD_PAUSE] == M4_GESTURE_ACTION_NONE, "pause inherits none");
}

static void test_warnings_keep_defaults(void)
{
	struct m4_settings s;

	m4_settings_defaults(&s);
	m4_settings_parse(&s,
			  "mode = turbo\n"
			  "gesture-next = explode\n"
			  "gesture-sideways = none\n"
			  "player = maybe\n"
			  "gesture-cooldown-ms = -5\n"
			  "gesture-cooldown-ms = 99999999\n"
			  "no equals sign here\n"
			  "= value\n",
			  "t");
	CHECK(s.n_warnings == 8, "8 warnings");
	CHECK(s.mode == M4_DAEMON_MODE_MANUAL, "bad mode keeps manual");
	CHECK(s.gesture[M4_PLAYER_CMD_NEXT] == M4_GESTURE_ACTION_CALL_TOGGLE, "bad action keeps default");
	CHECK(s.player_enabled, "bad bool keeps default");
	CHECK(s.gesture_cooldown_ms == 300, "bad cooldown keeps default");
	CHECK(strstr(s.first_warning, "t:1:") != NULL, "first warning has origin:line");
}

static void test_load_file(void)
{
	char dir[] = "/tmp/m4d-test-settings.XXXXXX";
	char path[256];
	struct m4_settings s;
	FILE *f;

	CHECK(mkdtemp(dir) != NULL, "mkdtemp");
	snprintf(path, sizeof(path), "%s/momentum4.conf", dir);

	CHECK(m4_settings_load(&s, path) == 1, "missing file -> 1");
	CHECK(s.mode == M4_DAEMON_MODE_MANUAL, "missing file -> manual");

	f = fopen(path, "w");
	CHECK(f != NULL, "fopen");
	if (f != NULL) {
		fputs("mode = observe\ngesture-previous = passthrough-exact", f); /* no final newline */
		fclose(f);
	}
	CHECK(m4_settings_load(&s, path) == 0, "load ok");
	CHECK(s.mode == M4_DAEMON_MODE_OBSERVE, "mode observe from file");
	CHECK(s.gesture[M4_PLAYER_CMD_PREVIOUS] == M4_GESTURE_ACTION_PASSTHROUGH_EXACT, "last line w/o newline");

	unlink(path);
	rmdir(dir);
}

static void test_config_path(void)
{
	char buf[256];

	setenv("MOMENTUM4_CONFIG", "/x/y.conf", 1);
	CHECK(m4_config_file_path(buf, sizeof(buf)) && strcmp(buf, "/x/y.conf") == 0, "override");
	unsetenv("MOMENTUM4_CONFIG");
	setenv("XDG_CONFIG_HOME", "/cfg", 1);
	CHECK(m4_config_file_path(buf, sizeof(buf)) && strcmp(buf, "/cfg/momentum4/momentum4.conf") == 0,
	      "xdg");
	unsetenv("XDG_CONFIG_HOME");
	setenv("HOME", "/home/u", 1);
	CHECK(m4_config_file_path(buf, sizeof(buf)) &&
		      strcmp(buf, "/home/u/.config/momentum4/momentum4.conf") == 0,
	      "home");
}

/* tap = mic: mic-toggle parses on both gesture-*
 * and hfp-* keys (control/gesture.c's m4_gesture_action_parse() table is
 * shared by both -- test_gesture_names() below already covers the generic
 * name<->value round-trip for every M4_GESTURE_ACTION_COUNT entry including
 * this one; this test is the settings.c-level, key-specific check that a
 * literal example config actually parses as intended). */
static void test_mic_toggle_action(void)
{
	struct m4_settings s;

	m4_settings_defaults(&s);
	m4_settings_parse(&s, "hfp-ata = mic-toggle\ngesture-previous = mic-toggle\n", "t");
	CHECK(s.n_warnings == 0, "mic-toggle parses without warnings on both key kinds");
	CHECK(s.hfp_ata == M4_GESTURE_ACTION_MIC_TOGGLE, "hfp-ata = mic-toggle");
	CHECK(s.gesture[M4_PLAYER_CMD_PREVIOUS] == M4_GESTURE_ACTION_MIC_TOGGLE, "gesture-previous = mic-toggle");
	CHECK(strcmp(m4_gesture_action_name(M4_GESTURE_ACTION_MIC_TOGGLE), "mic-toggle") == 0, "name string");
}

static void test_gesture_names(void)
{
	m4_player_cmd_t c;
	m4_gesture_action_t a;

	for (int i = 0; i < M4_PLAYER_CMD_COUNT; i++) {
		CHECK(m4_player_cmd_from_method(m4_player_cmd_method((m4_player_cmd_t)i), &c) == 0 &&
			      c == (m4_player_cmd_t)i,
		      "method round-trip");
	}
	CHECK(m4_player_cmd_from_method("play", &c) < 0, "method names are case-sensitive (D-Bus)");
	for (int i = 0; i < M4_GESTURE_ACTION_COUNT; i++) {
		CHECK(m4_gesture_action_parse(m4_gesture_action_name((m4_gesture_action_t)i), &a) == 0 &&
			      a == (m4_gesture_action_t)i,
		      "action round-trip");
	}
	CHECK(m4_gesture_action_parse("Call-Toggle", &a) == 0 && a == M4_GESTURE_ACTION_CALL_TOGGLE, "case");
	CHECK(m4_gesture_action_parse("ignore", &a) == 0 && a == M4_GESTURE_ACTION_NONE, "alias");
}

static void test_status_policy(void)
{
	m4_player_status_policy_t st;

	CHECK(m4_player_status_initial(M4_PLAYER_STATUS_FOLLOW) == M4_PLAYER_STATUS_PLAYING, "follow starts playing");
	st = M4_PLAYER_STATUS_PLAYING;
	st = m4_player_status_after(M4_PLAYER_STATUS_FOLLOW, st, M4_PLAYER_CMD_PAUSE);
	CHECK(st == M4_PLAYER_STATUS_PAUSED, "follow: pause -> paused");
	st = m4_player_status_after(M4_PLAYER_STATUS_FOLLOW, st, M4_PLAYER_CMD_NEXT);
	CHECK(st == M4_PLAYER_STATUS_PAUSED, "follow: next keeps");
	st = m4_player_status_after(M4_PLAYER_STATUS_FOLLOW, st, M4_PLAYER_CMD_PLAYPAUSE);
	CHECK(st == M4_PLAYER_STATUS_PLAYING, "follow: playpause flips");
	st = m4_player_status_after(M4_PLAYER_STATUS_FOLLOW, st, M4_PLAYER_CMD_STOP);
	CHECK(st == M4_PLAYER_STATUS_STOPPED, "follow: stop");
	CHECK(m4_player_status_after(M4_PLAYER_STATUS_STOPPED, M4_PLAYER_STATUS_STOPPED, M4_PLAYER_CMD_PLAY) ==
		      M4_PLAYER_STATUS_STOPPED,
	      "stopped policy is fixed");
	CHECK(strcmp(m4_player_status_mpris(M4_PLAYER_STATUS_PAUSED), "Paused") == 0, "mpris string");

	CHECK(strcmp(m4_gesture_passthrough_method(M4_GESTURE_ACTION_PASSTHROUGH, M4_PLAYER_CMD_PLAY),
		     "PlayPause") == 0,
	      "passthrough: Play -> PlayPause");
	CHECK(strcmp(m4_gesture_passthrough_method(M4_GESTURE_ACTION_PASSTHROUGH, M4_PLAYER_CMD_NEXT), "Next") == 0,
	      "passthrough: Next stays");
	CHECK(strcmp(m4_gesture_passthrough_method(M4_GESTURE_ACTION_PASSTHROUGH_EXACT, M4_PLAYER_CMD_PAUSE),
		     "Pause") == 0,
	      "passthrough-exact: Pause stays");
	CHECK(m4_gesture_passthrough_method(M4_GESTURE_ACTION_CALL_TOGGLE, M4_PLAYER_CMD_NEXT) == NULL,
	      "not a passthrough");
}

static void test_pick(void)
{
	struct m4_mpris_candidate c[3];

	memset(c, 0, sizeof(c));
	snprintf(c[0].name, sizeof(c[0].name), "org.mpris.MediaPlayer2.zeta");
	snprintf(c[0].status, sizeof(c[0].status), "Paused");
	snprintf(c[1].name, sizeof(c[1].name), "org.mpris.MediaPlayer2.beta");
	snprintf(c[1].status, sizeof(c[1].status), "Stopped");
	snprintf(c[2].name, sizeof(c[2].name), "org.mpris.MediaPlayer2.alpha");
	snprintf(c[2].status, sizeof(c[2].status), "Playing");
	CHECK(m4_passthrough_pick(c, 3) == 2, "playing wins");
	snprintf(c[2].status, sizeof(c[2].status), "Stopped");
	CHECK(m4_passthrough_pick(c, 3) == 0, "then paused");
	c[0].status[0] = '\0';
	CHECK(m4_passthrough_pick(c, 3) == 2, "then first by name");
	CHECK(m4_passthrough_pick(c, 0) == -1, "none");
}

/* player status after reconnect */
static void test_player_resync(void)
{
	struct m4_settings s;
	m4_player_resync_t r;
	static const m4_player_cmd_t flips[] = { M4_PLAYER_CMD_PLAY, M4_PLAYER_CMD_PAUSE, M4_PLAYER_CMD_PLAYPAUSE,
						 M4_PLAYER_CMD_STOP };

	CHECK(m4_player_resync_parse("on", &r) == 0 && r == M4_PLAYER_RESYNC_ON, "on");
	CHECK(m4_player_resync_parse("Connect", &r) == 0 && r == M4_PLAYER_RESYNC_CONNECT, "connect (case)");
	CHECK(m4_player_resync_parse("off", &r) == 0 && r == M4_PLAYER_RESYNC_OFF, "off");
	CHECK(m4_player_resync_parse("no", &r) == 0 && r == M4_PLAYER_RESYNC_OFF, "no -> off");
	CHECK(m4_player_resync_parse("sometimes", &r) < 0, "bad value");
	CHECK(strcmp(m4_player_resync_name(M4_PLAYER_RESYNC_CONNECT), "connect") == 0, "name");

	m4_settings_defaults(&s);
	m4_settings_parse(&s, "player-resync = connect\n", "t");
	CHECK(s.player_resync == M4_PLAYER_RESYNC_CONNECT && s.n_warnings == 0, "flat: player-resync = connect");
	m4_settings_parse(&s, "player-resync = maybe\n", "t");
	CHECK(s.player_resync == M4_PLAYER_RESYNC_CONNECT && s.n_warnings == 1, "flat: bad value -> warning, kept");

	/* the live config is the sectioned format (common/m4-conf registry) */
	{
		char dir[] = "/tmp/m4d-test-settings-rs.XXXXXX";
		char path[256];
		FILE *f;

		CHECK(mkdtemp(dir) != NULL, "mkdtemp");
		snprintf(path, sizeof(path), "%s/momentum4.conf", dir);
		f = fopen(path, "w");
		CHECK(f != NULL, "fopen");
		if (f != NULL) {
			fputs("[control]\nplayer-status = playing\nplayer-resync = off\n", f);
			fclose(f);
		}
		CHECK(m4_settings_load(&s, path) == 0, "sectioned load ok");
		CHECK(s.player_resync == M4_PLAYER_RESYNC_OFF && s.player_status == M4_PLAYER_STATUS_PLAYING,
		      "[control] player-resync = off");
		f = fopen(path, "w");
		if (f != NULL) {
			fputs("[control]\nplayer-status = playing\n", f);
			fclose(f);
		}
		CHECK(m4_settings_load(&s, path) == 0 && s.player_resync == M4_PLAYER_RESYNC_ON,
		      "sectioned, key absent -> registry default on");
		unlink(path);
		rmdir(dir);
	}

	for (size_t i = 0; i < sizeof(flips) / sizeof(flips[0]); i++) {
		CHECK(m4_player_resync_after_cmd(M4_PLAYER_RESYNC_ON, M4_PLAYER_STATUS_PLAYING, flips[i]),
		      "on + fixed playing + play/pause/stop -> resync");
		CHECK(m4_player_resync_after_cmd(M4_PLAYER_RESYNC_ON, M4_PLAYER_STATUS_PAUSED, flips[i]),
		      "on + fixed paused -> resync");
		CHECK(!m4_player_resync_after_cmd(M4_PLAYER_RESYNC_ON, M4_PLAYER_STATUS_FOLLOW, flips[i]),
		      "follow: status changes by itself");
		CHECK(!m4_player_resync_after_cmd(M4_PLAYER_RESYNC_ON, M4_PLAYER_STATUS_STOPPED, flips[i]),
		      "stopped: nothing to re-send");
		CHECK(!m4_player_resync_after_cmd(M4_PLAYER_RESYNC_CONNECT, M4_PLAYER_STATUS_PLAYING, flips[i]),
		      "connect: not after commands");
		CHECK(!m4_player_resync_after_cmd(M4_PLAYER_RESYNC_OFF, M4_PLAYER_STATUS_PLAYING, flips[i]), "off");
	}
	CHECK(!m4_player_resync_after_cmd(M4_PLAYER_RESYNC_ON, M4_PLAYER_STATUS_PLAYING, M4_PLAYER_CMD_NEXT),
	      "Next does not flip M4's belief");
	CHECK(!m4_player_resync_after_cmd(M4_PLAYER_RESYNC_ON, M4_PLAYER_STATUS_PLAYING, M4_PLAYER_CMD_PREVIOUS),
	      "Previous does not flip M4's belief");
}

int main(void)
{
	test_defaults();
	test_parse_example();
	test_hfp_and_earcon_delay();
	test_mic_toggle_action();
	test_mic_on_enter();
	test_inheritance();
	test_warnings_keep_defaults();
	test_load_file();
	test_config_path();
	test_gesture_names();
	test_status_policy();
	test_player_resync();
	test_pick();

	printf("test_settings: %d/%d checks passed\n", checks - failures, checks);
	return failures == 0 ? 0 : 1;
}
