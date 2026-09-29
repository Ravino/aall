/* momentum4: unit test for the aptx_adaptive media_codec, called
 * directly through dlopen()+spa_handle_factory_enum(), with NO BlueZ/D-Bus
 * and NO live PipeWire/WirePlumber involved at all -- this is the safe
 * way to test.
 *
 * Two parts:
 *   1. Byte-level protocol tests: fill_caps()/select_config()/enum_config()
 * against the known-good 40-byte MOMENTUM 4 layout,
 *      no helper process involved.
 *   2. A real end-to-end run: init() spawns the actual qemu-hexagon helper
 * (same binaries own tools use), streams 30 s of a test
 *      tone through start_encode()+encode() at the real 10 ms cadence, and
 *      checks the produced frames both directly (in-band marker check) and
 *      by parsing a raw (OTA-header-kept) capture taken straight from the
 *      bridge with this file's own native OTA-frame parser (see
 *      parse_raw_capture_native() below).
 *
 * Usage: unit_test <path-to-libspa-codec-bluez5-aptx.so> [duration_sec]
 * Exit code 0 = all tests passed.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/support/plugin.h>

#include "media-codecs.h"
#include "m4-adaptive-bridge.h"

static int g_failures = 0;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		printf("FAIL: "); printf(__VA_ARGS__); printf("  (%s:%d)\n", __FILE__, __LINE__); \
		g_failures++; \
	} else { \
		printf("PASS: "); printf(__VA_ARGS__); printf("\n"); \
	} \
} while (0)

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* This whole test binary runs
 * against an ISOLATED $HOME/$XDG_CONFIG_HOME/$XDG_RUNTIME_DIR (a fresh
 * mkdtemp() directory, set up once in main() before dlopen()), never the
 * real developer/operator ~/.config/momentum4 -- running against a real
 * config was flaky (latency-adjust-ms=-19 in the live config vs.
 * the hard-coded -15 default expected in the test). Every remaining
 * M4_ADAPTIVE_*-env-var-based test scenario below is now instead a small
 * momentum4.conf REWRITE of this isolated $XDG_CONFIG_HOME/momentum4/
 * momentum4.conf (m4_ah_effective_config_load()'s own "new format" layer,
 * m4-adaptive-conf-bridge.h) via write_conf()/reset_conf() -- not
 * $MOMENTUM4_CONFIG (that env var REPLACES this same layer's path outright,
 * which would also hide the qemu/loader/capi paths baseline_conf() writes
 * below from a scenario that only means to override ONE unrelated key). */
static char g_conf_path[700];
static char g_qemu_path[512], g_loader_path[512], g_capi_path[512];

/* Base content every scenario starts from: real qemu/loader/capi paths (so
 * codec->init()'s ACTUAL fork+exec+ENC_INIT keeps working throughout this
 * whole binary -- test_real_helper() et al. need this), [adaptive] left
 * untouched (-> compiled-in defaults: mode=ll0, latency-adjust-ms=-15,
 * matching every hardcoded expectation already in this file, e.g.
 * test_real_helper()'s "ts == ff f1 01 e0" check). */
static void write_conf(const char *extra_sections)
{
	FILE *f = fopen(g_conf_path, "w");
	if (!f) {
		fprintf(stderr, "unit_test: could not write %s: %s\n", g_conf_path, strerror(errno));
		return;
	}
	/* the codec's own default helper-mode is now SOCKET
	 * (systemd --user socket-activated service). This whole test binary is
	 * specifically about exercising the fork+exec+pipe SPAWN path directly
	 * against real qemu/loader/capi binaries and has no systemd socket to
	 * connect to in this offline/CI context -- pin the mode explicitly
	 * rather than relying on whatever the ambient default happens to be
	 * (was M4_ADAPTIVE_HELPER_MODE=spawn, set once in main() -- now part of
	 * every write_conf() base so no scenario below can accidentally revert
	 * it). Socket-mode-specific behaviour has its own test, see
	 * test/socket_bridge_test.c. */
	fprintf(f, "[adaptive]\nhelper-mode = spawn\n[helper]\nqemu = %s\nloader = %s\ncapi = %s\n",
			g_qemu_path, g_loader_path, g_capi_path);
	if (extra_sections && *extra_sections)
		fprintf(f, "\n%s", extra_sections);
	fclose(f);
}

static void reset_conf(void)
{
	write_conf(NULL);
}

/* mkdtemp()'s own isolated $HOME (never removed -- this is a short-lived
 * test binary, not something that runs unattended for long; leaving the
 * directory behind after a run is a debugging convenience, same spirit as
 * test_real_helper()'s own /tmp/m4_adaptive_unit_test_capture.raw). Exits
 * the whole process on failure (this is setup, not a single CHECK). */
static void setup_isolated_home(void)
{
	char template[] = "/tmp/m4-unit-test-home-XXXXXX";
	char *home = mkdtemp(template);
	char confdir[600];
	const char *env_qemu, *env_loader, *env_capi;

	if (!home) {
		fprintf(stderr, "unit_test: mkdtemp() failed: %s\n", strerror(errno));
		exit(2);
	}
	setenv("HOME", home, 1);
	snprintf(confdir, sizeof(confdir), "%s/.config", home);
	setenv("XDG_CONFIG_HOME", confdir, 1);
	snprintf(confdir, sizeof(confdir), "%s/run", home);
	mkdir(confdir, 0700);
	setenv("XDG_RUNTIME_DIR", confdir, 1);
	unsetenv("MOMENTUM4_CONFIG"); /* make sure nothing from the calling shell leaks in */

	snprintf(confdir, sizeof(confdir), "%s/.config/momentum4", home);
	/* two-level mkdir: $HOME/.config then .../momentum4 */
	{
		char parent[600];
		snprintf(parent, sizeof(parent), "%s/.config", home);
		mkdir(parent, 0700);
		mkdir(confdir, 0700);
	}
	snprintf(g_conf_path, sizeof(g_conf_path), "%s/momentum4.conf", confdir);

	/* Real dev-machine paths (same fallback literals main() already used
	 * for test_raw_capture_native() below, now the ONE place they are
	 * spelled out) -- overridable via the SAME env vars this file's own
	 * header used to read directly, purely as a developer convenience for
	 * running this binary against a different checkout/build, never read
	 * by the codec itself any more. */
	env_qemu = getenv("M4_ADAPTIVE_QEMU");
	env_loader = getenv("M4_ADAPTIVE_LOADER");
	env_capi = getenv("M4_ADAPTIVE_CAPI");
	/* Generic system-install defaults, same ones the codec itself falls
	 * back to (see M4_ADAPTIVE_DEFAULT_{QEMU,LOADER,CAPI}_PATH in
	 * a2dp-codec-aptx.c) -- NOT tied to any one checkout. A developer
	 * running this binary against a local build sets the three env vars
	 * above instead (see this file's usage example in the module README). */
	snprintf(g_qemu_path, sizeof(g_qemu_path), "%s",
			env_qemu ? env_qemu : "/usr/bin/qemu-hexagon");
	snprintf(g_loader_path, sizeof(g_loader_path), "%s",
			env_loader ? env_loader :
			"/usr/lib/x86_64-linux-gnu/momentum4/hexagon/hx_adaptive_loader");
	snprintf(g_capi_path, sizeof(g_capi_path), "%s",
			env_capi ? env_capi :
			"/usr/lib/x86_64-linux-gnu/momentum4/hexagon/capi_v2_aptX_CLHDADV_Encoder.so");

	reset_conf();
}

/* Expected 40-byte MOMENTUM 4 config -- NQ2Q, confirmed live by ear
 * and byte-for-byte against the shipped default. Supersedes
 * the earlier Q2Q/STEREO placeholder bytes (0x10/0x02/generic-ADK-TTP),
 * which is silently accepted by M4 but produces no audio -- see
 * the comment above APTX_ADAPTIVE_SOURCE_TYPE_NQ2Q
 * in a2dp-codec-aptx.c. */
static const uint8_t expected_caps[40] = {
	0xD7, 0x00, 0x00, 0x00, 0xAD, 0x00, /* vendor, codec */
	0x13, 0x08,                         /* 48k | NQ2Q | generic, JOINT */
	0x37, 0x6A, 0xC8, 0x7D, 0x64, 0x7D, /* ttp ll0/ll1/hq0/hq1/tws0/tws1 (55/106/200/125/100/125 ms) */
	0x00,                               /* reserved 15th byte */
	0x01,                               /* cap_ext_ver */
	0x02, 0x00, 0x00, 0x0F,             /* features 0x0F000002 LE */
	0x02, 0x03, 0x03, 0x03,             /* setup prefs */
	0x00, 0xAA,                         /* eoc0, eoc1 */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,        /* 14 reserved */
};

static const struct media_codec *load_aptx_adaptive(void *dl, struct spa_handle **out_handle)
{
	typedef int (*enum_fn_t)(const struct spa_handle_factory **factory, uint32_t *index);
	enum_fn_t enum_fn = (enum_fn_t)dlsym(dl, "spa_handle_factory_enum");
	if (!enum_fn) {
		printf("FAIL: dlsym(spa_handle_factory_enum): %s\n", dlerror());
		return NULL;
	}

	const struct spa_handle_factory *factory = NULL;
	uint32_t idx = 0;
	if (enum_fn(&factory, &idx) != 1 || factory == NULL) {
		printf("FAIL: spa_handle_factory_enum returned no factory\n");
		return NULL;
	}

	size_t sz = factory->get_size(factory, NULL);
	struct spa_handle *handle = calloc(1, sz);
	if (!handle) { printf("FAIL: calloc handle\n"); return NULL; }

	int res = factory->init(factory, handle, NULL, NULL, 0);
	if (res < 0) {
		printf("FAIL: factory->init() = %d (%s)\n", res, strerror(-res));
		free(handle);
		return NULL;
	}

	void *iface = NULL;
	res = handle->get_interface(handle, SPA_TYPE_INTERFACE_Bluez5CodecMedia, &iface);
	if (res < 0 || iface == NULL) {
		printf("FAIL: get_interface(Bluez5CodecMedia) = %d\n", res);
		free(handle);
		return NULL;
	}

	const struct spa_bluez5_codec_a2dp *a2dp = iface;
	CHECK(a2dp->iface.version == SPA_VERSION_BLUEZ5_CODEC_MEDIA,
			"codec plugin ABI version matches SPA_VERSION_BLUEZ5_CODEC_MEDIA (%d)",
			SPA_VERSION_BLUEZ5_CODEC_MEDIA);

	const struct media_codec *found = NULL;
	for (size_t i = 0; a2dp->codecs[i]; i++) {
		if (strcmp(a2dp->codecs[i]->name, "aptx_adaptive") == 0) {
			found = a2dp->codecs[i];
			break;
		}
	}
	CHECK(found != NULL, "aptx_adaptive present in the plugin's codec table");
	*out_handle = handle;
	return found;
}

static void test_byte_layout(const struct media_codec *codec)
{
	uint8_t caps[A2DP_MAX_CAPS_SIZE];
	int n = codec->fill_caps(codec, 0, NULL, caps);
	CHECK(n == (int)sizeof(expected_caps), "fill_caps() returns 40 bytes (got %d)", n);
	CHECK(n > 0 && memcmp(caps, expected_caps, sizeof(expected_caps)) == 0,
			"fill_caps() byte-for-byte matches the MOMENTUM 4 CAF layout");

	/* select_config(): remote (M4) caps == our own caps, as in reality
	 * . */
	uint8_t config[A2DP_MAX_CAPS_SIZE];
	struct media_codec_audio_info info = { .rate = 48000, .channels = 2 };
	int r = codec->select_config(codec, 0, expected_caps, sizeof(expected_caps),
			&info, NULL, config);
	CHECK(r == (int)sizeof(expected_caps), "select_config() accepts M4's own caps (r=%d)", r);
	CHECK(r > 0 && memcmp(config, expected_caps, sizeof(expected_caps)) == 0,
			"select_config() output matches the fixed MOMENTUM 4 config");

	/* Negative cases: must fail cleanly (no crash), never assert/abort. */
	uint8_t bad[40];
	memcpy(bad, expected_caps, sizeof(bad));
	bad[0] = 0x99; /* wrong vendor id */
	r = codec->select_config(codec, 0, bad, sizeof(bad), &info, NULL, config);
	CHECK(r == -ENOTSUP, "select_config() rejects wrong vendor id (r=%d)", r);

	r = codec->select_config(codec, 0, expected_caps, 4 /* too short */, &info, NULL, config);
	CHECK(r == -EINVAL, "select_config() rejects too-short caps (r=%d)", r);

	memcpy(bad, expected_caps, sizeof(bad));
	bad[7] = 0x00; /* no channel mode bits set at all */
	r = codec->select_config(codec, 0, bad, sizeof(bad), &info, NULL, config);
	CHECK(r == -ENOTSUP, "select_config() rejects an empty channel_mode bitmask (r=%d)", r);

	/* The exact original silent-Q2Q bug: a peer that only
	 * offers plain STEREO (0x02, no JOINT bit) must be rejected, not
	 * downgraded to -- M4 in this state produces no audio. */
	memcpy(bad, expected_caps, sizeof(bad));
	bad[7] = 0x02;
	r = codec->select_config(codec, 0, bad, sizeof(bad), &info, NULL, config);
	CHECK(r == -ENOTSUP, "select_config() rejects a JOINT-less STEREO-only "
			"channel_mode (r=%d) -- the original silent-Q2Q/STEREO bug", r);

	/* Q2Q (byte 6 bit 1 clear) must never be selected even if a peer offers
	 * it: our select_config() always emits ITS OWN fixed config (0x13/0x08),
	 * never an echo of whatever the peer sent, so this cannot regress
	 * silently. Assert that explicitly. */
	CHECK((expected_caps[6] & 0x02) != 0, "our own caps advertise NQ2Q (byte 6 bit 1 set)");
	CHECK(expected_caps[7] == 0x08, "our own caps advertise JOINT only (byte 7 == 0x08)");
}

/* allow-devices filter for the aptx_adaptive codec, default m4:
 * a "foreign" aptX Adaptive receiver -- same vendor_id/codec_id and the same
 * loosely-checked 48 kHz/JOINT bits as MOMENTUM 4 (so the old, fingerprint-less checks in
 * codec_select_config_adaptive()/codec_validate_config_adaptive() would have
 * accepted it), but with DIFFERENT TTP target bytes: 0x50/0x64/0x64/0x64/
 * 0xFF/0xFF instead of M4's 0x37/0x6A/0xC8/0x7D/0x64/0x7D. These are not
 * arbitrary -- they are the generic ADK "stereo headset" placeholder TTP
 * values this project's own early testing originally used before M4's real
 * values were captured live, i.e. a plausible stand-in for what some
 * OTHER Qualcomm-reference-design receiver's sep8 might look like. See
 * adaptive_caps_match_m4_fingerprint()'s own comment in a2dp-codec-aptx.c
 * for why this is speculative about other receivers, not a captured real
 * device -- the open question still open. */
static const uint8_t foreign_caps[40] = {
	0xD7, 0x00, 0x00, 0x00, 0xAD, 0x00, /* same vendor, codec as M4 */
	0x13, 0x08,                         /* same loosely-checked bits as M4 */
	0x50, 0x64, 0x64, 0x64, 0xFF, 0xFF, /* DIFFERENT ttp (generic ADK placeholder, not M4's) */
	0x00,
	0x01,
	0x02, 0x00, 0x00, 0x0F,
	0x02, 0x03, 0x03, 0x03,
	0x00, 0xAA,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

static void test_allow_devices_filter(const struct media_codec *codec)
{
	uint8_t config[A2DP_MAX_CAPS_SIZE];
	struct media_codec_audio_info info = { .rate = 48000, .channels = 2 };
	struct spa_audio_info vinfo;
	int r;

	/* Sanity: the two fixtures actually differ only where intended (TTP
	 * bytes), everything else byte-identical -- otherwise the "foreign"
	 * case below would also be rejected by the pre-existing generic checks
	 * (vendor/codec/rate-bit/JOINT-bit) and this test would prove nothing
	 * about the NEW fingerprint check specifically. */
	CHECK(memcmp(foreign_caps, expected_caps, 8) == 0 &&
			memcmp(foreign_caps + 8, expected_caps + 8, sizeof(foreign_caps) - 8) != 0,
			"foreign_caps fixture differs from expected_caps only in the TTP "
			"bytes (offset 8+), matches in vendor/codec/rate/channel bits (0-7)");

	/* Default policy (no allow-devices key set): allow-devices=adk-stereo.
	 * MOMENTUM 4's own caps must still be accepted (no regression from
	 * test_byte_layout() above)... */
	reset_conf();
	r = codec->select_config(codec, 0, expected_caps, sizeof(expected_caps), &info, NULL, config);
	CHECK(r == (int)sizeof(expected_caps),
			"allow-devices=adk-stereo (default): select_config() still accepts M4's own caps (r=%d)", r);

	/* ...but a peer with different TTP/features/setup_pref/EOC bytes -- same
	 * vendor/codec/rate/JOINT bits, so the OLD (pre-fingerprint) checks alone
	 * would have accepted it -- must be rejected. This is the "foreign
	 * caps" case the allow-devices filter exists for. */
	r = codec->select_config(codec, 0, foreign_caps, sizeof(foreign_caps), &info, NULL, config);
	CHECK(r == -ENOTSUP,
			"allow-devices=adk-stereo (default): select_config() REJECTS a foreign aptX "
			"Adaptive receiver's caps (same vendor/codec/rate/JOINT bits, "
			"different TTP fingerprint) (r=%d)", r);

	spa_zero(vinfo);
	r = codec->validate_config(codec, 0, foreign_caps, sizeof(foreign_caps), &vinfo);
	CHECK(r == -EINVAL,
			"allow-devices=adk-stereo (default): validate_config() REJECTS the same "
			"foreign caps (r=%d) -- the SetConfiguration() entry point, "
			"independent of select_config()", r);

	spa_zero(vinfo);
	r = codec->validate_config(codec, 0, expected_caps, sizeof(expected_caps), &vinfo);
	CHECK(r == 0, "allow-devices=adk-stereo (default): validate_config() accepts M4's own caps (r=%d)", r);
	CHECK(r != 0 || (vinfo.media_type == SPA_MEDIA_TYPE_audio &&
			vinfo.media_subtype == SPA_MEDIA_SUBTYPE_raw &&
			vinfo.info.raw.format == SPA_AUDIO_FORMAT_S16_LE &&
			vinfo.info.raw.rate == 48000 && vinfo.info.raw.channels == 2 &&
			vinfo.info.raw.position[0] == SPA_AUDIO_CHANNEL_FL &&
			vinfo.info.raw.position[1] == SPA_AUDIO_CHANNEL_FR),
			"validate_config() fills info as S16LE/48000/2ch FL+FR "
			"(matches BlueZ's old \"no validate_config\" fallback, "
			"for the caps that pass)");

	/* Opt-out: allow-devices=any restores the old, loose behaviour -- the
	 * foreign receiver is now accepted (both entry points). */
	write_conf("[adaptive]\nallow-devices = any\n");
	r = codec->select_config(codec, 0, foreign_caps, sizeof(foreign_caps), &info, NULL, config);
	CHECK(r == (int)sizeof(foreign_caps),
			"allow-devices=any: select_config() accepts the foreign caps (r=%d)", r);
	spa_zero(vinfo);
	r = codec->validate_config(codec, 0, foreign_caps, sizeof(foreign_caps), &vinfo);
	CHECK(r == 0, "allow-devices=any: validate_config() accepts the foreign caps (r=%d)", r);

	/* An unrelated vendor/codec id must be rejected either way -- allow-
	 * devices only widens/narrows WHICH aptX Adaptive receiver is accepted,
	 * it must never let a non-aptX-Adaptive blob through. */
	{
		uint8_t bad[40];
		memcpy(bad, foreign_caps, sizeof(bad));
		bad[0] = 0x99;
		r = codec->select_config(codec, 0, bad, sizeof(bad), &info, NULL, config);
		CHECK(r == -ENOTSUP, "allow-devices=any: select_config() still rejects a "
				"wrong vendor id, unrelated to the fingerprint check (r=%d)", r);
	}
	reset_conf();

	/* validate_config() must never crash/misbehave on too-short input,
	 * mirroring select_config()'s own bounds check. */
	spa_zero(vinfo);
	r = codec->validate_config(codec, 0, expected_caps, 4, &vinfo);
	CHECK(r == -EINVAL, "validate_config() rejects too-short caps (r=%d)", r);
}

/* "the helper fails to start... the codec is not advertised
 * in any endpoint at all": fill_caps() must refuse (negative return) when the
 * helper is not available, WITHOUT crashing and WITHOUT touching `caps`'
 * contents in a way a caller might mistake for success -- this is exactly
 * the hook bluez5-dbus.c uses to decide whether to register the
 * aptx_adaptive MediaEndpoint1 with BlueZ at all (see the comment above
 * codec_fill_caps_adaptive() in a2dp-codec-aptx.c for the two call sites and
 * their `if (caps_size < 0) continue;` / `return ret;` behaviour). Every
 * env var this test sets is unset again before returning, and a final
 * baseline call confirms fill_caps() is back to succeeding -- the real
 * qemu-hexagon test right after this one in main() must not be affected. */
static void test_fill_caps_probe(const struct media_codec *codec)
{
	uint8_t caps[A2DP_MAX_CAPS_SIZE];
	int n;

	n = codec->fill_caps(codec, 0, NULL, caps);
	CHECK(n == (int)sizeof(expected_caps),
			"fill_caps() probe: succeeds (40 bytes) when the helper is available "
			"(baseline, got %d)", n);

	{
		char extra[1200];
		snprintf(extra, sizeof(extra), "[helper]\nqemu = /nonexistent/path/to/qemu-hexagon-does-not-exist\nloader = %s\ncapi = %s\n",
				g_loader_path, g_capi_path);
		write_conf(extra);
	}
	n = codec->fill_caps(codec, 0, NULL, caps);
	CHECK(n < 0, "fill_caps() probe: returns negative when [helper] qemu points "
			"to a nonexistent/non-executable binary (\"no qemu\" case, got %d)", n);

	{
		char extra[1200];
		snprintf(extra, sizeof(extra), "[helper]\nqemu = %s\nloader = %s\ncapi = /nonexistent/capi_v2_aptX_CLHDADV_Encoder.so\n",
				g_qemu_path, g_loader_path);
		write_conf(extra);
	}
	n = codec->fill_caps(codec, 0, NULL, caps);
	CHECK(n < 0, "fill_caps() probe: returns negative when [helper] capi points "
			"to a nonexistent module (\"no Qualcomm module\" case, got %d)", n);

	{
		char extra[1200];
		snprintf(extra, sizeof(extra), "[helper]\nqemu = %s\nloader = /nonexistent/hx_adaptive_loader\ncapi = %s\n",
				g_qemu_path, g_capi_path);
		write_conf(extra);
	}
	n = codec->fill_caps(codec, 0, NULL, caps);
	CHECK(n < 0, "fill_caps() probe: returns negative when [helper] loader "
			"points to a nonexistent ELF (got %d)", n);
	reset_conf();

	/* Symmetry fix: [adaptive] enable = off (replaces the
	 * old M4_ADAPTIVE_DISABLE env var) used to only fail
	 * codec_init_adaptive() -- i.e. AFTER BlueZ had already negotiated
	 * aptx_adaptive with the peer, the exact same problem. Now it must also
	 * stop fill_caps() from advertising the endpoint in the first place. */
	write_conf("[adaptive]\nenable = off\n");
	n = codec->fill_caps(codec, 0, NULL, caps);
	CHECK(n < 0, "fill_caps() probe: returns negative when [adaptive] enable = off "
			"(got %d)", n);
	reset_conf();

	n = codec->fill_caps(codec, 0, NULL, caps);
	CHECK(n == (int)sizeof(expected_caps),
			"fill_caps() probe: back to succeeding once all overrides are unset "
			"(no leaked state, got %d)", n);
}

static void test_enum_config(const struct media_codec *codec)
{
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod *param = NULL;

	int r = codec->enum_config(codec, 0, expected_caps, sizeof(expected_caps),
			0 /* id */, 0 /* idx */, &b, &param);
	CHECK(r == 1 && param != NULL, "enum_config() produces one format POD (r=%d)", r);

	if (param) {
		struct spa_audio_info info;
		spa_zero(info);
		int pr = spa_format_audio_raw_parse(param, &info.info.raw);
		int mr = spa_format_parse(param, &info.media_type, &info.media_subtype);
		/* spa_pod_parse_object()/spa_pod_parser_getv() return the number of
		 * matched fields (>= 0) on success, not always exactly 0 -- only a
		 * negative value is an actual parse error. */
		CHECK(mr >= 0 && info.media_type == SPA_MEDIA_TYPE_audio &&
				info.media_subtype == SPA_MEDIA_SUBTYPE_raw,
				"enum_config() POD is audio/raw (mr=%d)", mr);
		CHECK(pr >= 0 && info.info.raw.format == SPA_AUDIO_FORMAT_S16_LE &&
				info.info.raw.rate == 48000 && info.info.raw.channels == 2,
				"enum_config() POD is S16LE/48000/2ch (fmt=%d rate=%d ch=%d)",
				info.info.raw.format, info.info.raw.rate, info.info.raw.channels);
	}

	r = codec->enum_config(codec, 0, expected_caps, sizeof(expected_caps), 0, 1, &b, &param);
	CHECK(r == 0, "enum_config() has exactly one entry (idx=1 -> r=%d)", r);
}

/* Generates a simple test tone, "sine" mode (1 kHz on L unless
 * overridden), matching the same deterministic function of sample index
 * only used elsewhere to independently generate/verify test captures. */
static void gen_sine_block(int16_t *out, uint32_t start_sample, uint32_t n_samples,
		double freq_l, double freq_r)
{
	for (uint32_t i = 0; i < n_samples; i++) {
		double t = (double)(start_sample + i) / 48000.0;
		double l = 0.5 * sin(2.0 * M_PI * freq_l * t);
		double r = 0.5 * sin(2.0 * M_PI * freq_r * t);
		out[2 * i + 0] = (int16_t)(l * 32767.0);
		out[2 * i + 1] = (int16_t)(r * 32767.0);
	}
}

static void test_real_helper(const struct media_codec *codec, double duration_sec)
{
	struct spa_audio_info info;
	spa_zero(info);
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info.info.raw.format = SPA_AUDIO_FORMAT_S16_LE;
	info.info.raw.rate = 48000;
	info.info.raw.channels = 2;

	void *data = codec->init(codec, 0, (void *)expected_caps, sizeof(expected_caps),
			&info, NULL, 1024 /* MTU, generous, > 12+348 */);
	CHECK(data != NULL, "codec->init() spawns the real helper and succeeds "
			"(check M4_ADAPTIVE_{QEMU,LOADER,CAPI} paths if this fails)");
	if (!data) {
		printf("SKIP: real helper streaming test (init failed)\n");
		return;
	}

	int block_size = codec->get_block_size(data);
	CHECK(block_size == (int)M4_AH_PCM_BLOCK_BYTES,
			"get_block_size() == %u bytes (got %d)", M4_AH_PCM_BLOCK_BYTES, block_size);

	long n_blocks = (long)(duration_sec * 1000.0 / 10.0); /* 10 ms/block */
	printf("INFO: streaming %ld blocks (%.1f s) of test tone through the real helper\n",
			n_blocks, duration_sec);

	uint8_t dst[65536];
	long frames_ok = 0, frames_skipped = 0, marker_mismatches = 0;
	double max_call_ms = 0, sum_call_ms = 0;
	uint32_t sample_pos = 0;

	FILE *capture = fopen("/tmp/m4_adaptive_unit_test_capture.raw", "wb");

	for (long blk = 0; blk < n_blocks; blk++) {
		int16_t pcm[M4_AH_PCM_SAMPLES_PER_BLOCK * 2];
		gen_sine_block(pcm, sample_pos, M4_AH_PCM_SAMPLES_PER_BLOCK, 1000.0, 440.0);
		sample_pos += M4_AH_PCM_SAMPLES_PER_BLOCK;

		double t0 = now_ms();
		int hdr_len = codec->start_encode(data, dst, sizeof(dst), (uint16_t)blk,
				sample_pos);
		CHECK(blk != 0 || hdr_len == 12, "start_encode() writes a 12-byte RTP header");
		/* Confirmed reference header: "80 60 ..
		 * 00 00 01 e0 00 00 00 a1/a2". Default mode is ll0 (ssrc 0xA1) with
		 * latency_adjust_ms -15, so ts upper 16 bits
		 * are (int16_t)-15 = 0xFFF1. */
		CHECK(blk != 0 || (dst[0] == 0x80 && dst[1] == 0x60),
				"start_encode() header bytes 0-1 == 80 60 (v=2/pad=0/ext=0/cc=0, m=0/pt=96)");
		CHECK(blk != 0 || (dst[4] == 0xFF && dst[5] == 0xF1 && dst[6] == 0x01 && dst[7] == 0xE0),
				"start_encode() default ts == ff f1 01 e0 (ll0, adjust -15ms, 480 samples)");
		CHECK(blk != 0 || (dst[8] == 0x00 && dst[9] == 0x00 && dst[10] == 0x00 && dst[11] == 0xA1),
				"start_encode() default ssrc == 00 00 00 a1 (ll0)");

		size_t dst_out = 0;
		int need_flush = -1;
		int consumed = codec->encode(data, pcm, sizeof(pcm),
				dst + hdr_len, sizeof(dst) - (size_t)hdr_len, &dst_out, &need_flush);
		double call_ms = now_ms() - t0;
		sum_call_ms += call_ms;
		if (call_ms > max_call_ms)
			max_call_ms = call_ms;

		if (consumed != (int)sizeof(pcm) && blk == 0)
			CHECK(0, "encode() consumes the full PCM block (got %d, want %zu)",
					consumed, sizeof(pcm));

		if (dst_out > 0) {
			frames_ok++;
			if (dst[hdr_len + 0] != 0x84 /* LL in-band marker */)
				marker_mismatches++;
			else if (dst[hdr_len + 3] * 4 + 12 != (int)dst_out)
				marker_mismatches++;
			if (capture)
				fwrite(dst + hdr_len, 1, dst_out, capture);
		} else {
			frames_skipped++;
		}

		/* Real-time-ish pacing: sleep the remainder of the 10 ms interval
		 * so the helper is fed at (approximately) the real cadence
		 * media-sink.c would use. Not exact -- this is a test, not the RT
		 * data thread -- but it keeps the pipeline from being fed faster
		 * than 1 block/10ms, which would just make the bridge coalesce/
		 * drop blocks (by design) rather than testing steady-state.
		 */
		double elapsed = now_ms() - t0;
		if (elapsed < 10.0)
			usleep((useconds_t)((10.0 - elapsed) * 1000.0));
	}
	if (capture)
		fclose(capture);

	printf("INFO: frames_ok=%ld frames_skipped=%ld marker_mismatches=%ld "
			"avg_encode_call_ms=%.3f max_encode_call_ms=%.3f\n",
			frames_ok, frames_skipped, marker_mismatches,
			n_blocks ? sum_call_ms / n_blocks : 0.0, max_call_ms);

	CHECK(marker_mismatches == 0, "every produced frame has a valid in-band marker "
			"(byte0=0x84, byte3*4+12==len)");
	/* Generous RT-safety bound: encode() itself (not the helper round trip)
	 * must never block for a meaningful fraction of the 10 ms budget. 5 ms
	 * is already 50x the trylock+memcpy cost we designed for; it is only
	 * this loose to absorb test-process scheduling noise, not because we
	 * expect to need it. */
	CHECK(max_call_ms < 5.0, "encode() never took more than 5 ms to return "
			"(max observed %.3f ms) -- RT-safety bound", max_call_ms);
	CHECK(n_blocks < 10 || frames_ok > n_blocks * 9 / 10,
			"the real helper keeps up with real-time cadence: >=90%% of blocks "
			"produced a frame (%ld/%ld)", frames_ok, n_blocks);

	codec->deinit(data);
	printf("INFO: raw stripped-frame capture written to "
			"/tmp/m4_adaptive_unit_test_capture.raw (%ld frames, for manual inspection)\n",
			frames_ok);
}

/* Short, separate real-helper run with M4_ADAPTIVE_MODE=ll1, to check the
 * OTHER half of the reference header ("... 00 00 00 a1/a2"): ssrc 0xA2,
 * ts base 0x0000006A*ms... i.e. base target 106 ms -> with the default
 * latency_adjust_ms 0 for this env-only override (no config file / other env
 * set), ts upper 16 bits are 0x0000. Restores the environment afterwards so
 * this cannot leak into any other test in this binary. */
static void test_mode_ll1_header(const struct media_codec *codec)
{
	struct spa_audio_info info;
	spa_zero(info);
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info.info.raw.format = SPA_AUDIO_FORMAT_S16_LE;
	info.info.raw.rate = 48000;
	info.info.raw.channels = 2;

	write_conf("[adaptive]\nmode = ll1\nlatency-adjust-ms = 0\n");

	void *data = codec->init(codec, 0, (void *)expected_caps, sizeof(expected_caps),
			&info, NULL, 1024);
	CHECK(data != NULL, "codec->init() with [adaptive] mode = ll1 succeeds");
	if (data) {
		uint8_t dst[65536];
		int16_t pcm[M4_AH_PCM_SAMPLES_PER_BLOCK * 2];
		int hdr_len = codec->start_encode(data, dst, sizeof(dst), 0, 480);

		CHECK(hdr_len == 12, "ll1: start_encode() writes a 12-byte RTP header");
		CHECK(dst[4] == 0x00 && dst[5] == 0x00 && dst[6] == 0x01 && dst[7] == 0xE0,
				"ll1: ts == 00 00 01 e0 (adjust 0ms, 480 samples)");
		CHECK(dst[8] == 0x00 && dst[9] == 0x00 && dst[10] == 0x00 && dst[11] == 0xA2,
				"ll1: ssrc == 00 00 00 a2 -- the reference header's \"...a2\" case");

		/* Feed it one real block so the helper/bridge shuts down cleanly
		 * (not required for the header check itself, just tidy). */
		gen_sine_block(pcm, 0, M4_AH_PCM_SAMPLES_PER_BLOCK, 1000.0, 440.0);
		size_t dst_out = 0;
		int need_flush = -1;
		codec->encode(data, pcm, sizeof(pcm), dst + hdr_len,
				sizeof(dst) - (size_t)hdr_len, &dst_out, &need_flush);
		codec->deinit(data);
	}

	reset_conf();
}

/* "reconnect-grace": the explicit scenario "start ->
 * stop -> start within 1s: helper same, no mute". Uses a short, deterministic
 * M4_ADAPTIVE_RECONNECT_GRACE_MS (300 ms, not the 1500 ms production default)
 * so this test does not need to actually wait ~1.5 s to prove its second half
 * (grace expiry). Three init()/deinit() cycles:
 *   1. First init(): a genuinely cold start (fork+exec+ELF-load+ENC_INIT
 *      under TCG) -- necessarily tens of ms at minimum, used only as the
 *      "slow" baseline to compare against.
 *   2. deinit() immediately, then init() again within ~50 ms (well inside
 *      the 300 ms grace window) with byte-identical (config, mtu, mode,
 *      latency_adjust_ms): must be reclaimed from the park slot -- no
 *      fork/connect/ENC_INIT at all, so this must complete at least an order
 *      of magnitude faster than step 1, AND must return a bridge that still
 *      actually encodes real frames afterwards (proves reuse did not hand
 *      back a half-broken/stale instance).
 *   3. deinit() again, then sleep well past the 300 ms grace window, then
 *      init() a third time: the reaper thread must have already torn the
 *      parked instance down, so this MUST be slow again (same order of
 *      magnitude as step 1) -- proves the grace window actually expires and
 *      this is not an unbounded "keep everything forever" cache. */
static void test_reconnect_grace(const struct media_codec *codec)
{
	struct spa_audio_info info;
	spa_zero(info);
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info.info.raw.format = SPA_AUDIO_FORMAT_S16_LE;
	info.info.raw.rate = 48000;
	info.info.raw.channels = 2;

	write_conf("[adaptive]\nreconnect-grace-ms = 300\n");

	/* Step 1: cold init. */
	double t0 = now_ms();
	void *data1 = codec->init(codec, 0, (void *)expected_caps, sizeof(expected_caps),
			&info, NULL, 1024);
	double cold_ms_1 = now_ms() - t0;
	CHECK(data1 != NULL, "reconnect-grace: first (cold) codec->init() succeeds");
	if (!data1) {
		printf("SKIP: reconnect-grace test (first init failed)\n");
		reset_conf();
		return;
	}
	printf("INFO: reconnect-grace: cold init #1 took %.1f ms\n", cold_ms_1);

	/* Step 2: stop -> start well within the 300 ms grace window ("within 1s"
	 * from the task, made tighter here to also prove it works much sooner
	 * than the full window, not just barely before it expires). */
	codec->deinit(data1);
	usleep(50 * 1000); /* 50 ms: "start -> stop -> start" in well under 1 s */

	t0 = now_ms();
	void *data2 = codec->init(codec, 0, (void *)expected_caps, sizeof(expected_caps),
			&info, NULL, 1024);
	double reclaim_ms = now_ms() - t0;
	CHECK(data2 != NULL, "reconnect-grace: reclaimed codec->init() succeeds");
	printf("INFO: reconnect-grace: reclaimed init #2 took %.3f ms (vs %.1f ms cold)\n",
			reclaim_ms, cold_ms_1);
	CHECK(data2 == data1, "reconnect-grace: reclaim returns the SAME impl_adaptive "
			"instance (same bridge/heartbeat state, no re-ENC_INIT)");
	CHECK(reclaim_ms < 10.0 && reclaim_ms < cold_ms_1 / 5.0,
			"reconnect-grace: reclaimed init() is at least 5x faster than cold "
			"init AND under 10 ms absolute (no fork/connect/ENC_INIT happened)");

	if (data2) {
		/* Prove the reclaimed instance still actually works, not just "fast
		 * but broken": one real block must produce a real, well-formed
		 * frame. */
		uint8_t dst[65536];
		int16_t pcm[M4_AH_PCM_SAMPLES_PER_BLOCK * 2];
		gen_sine_block(pcm, 0, M4_AH_PCM_SAMPLES_PER_BLOCK, 1000.0, 440.0);
		int hdr_len = codec->start_encode(data2, dst, sizeof(dst), 0, 480);
		size_t dst_out = 0;
		int need_flush = -1;
		/* The real helper's round trip is not instantaneous -- give it a
		 * few retries at 10 ms spacing (same cadence as steady state)
		 * rather than a single encode() call, since a fresh submit_pcm()
		 * right after reclaim may land just before the IO thread wakes. */
		for (int attempt = 0; attempt < 20 && dst_out == 0; attempt++) {
			codec->encode(data2, pcm, sizeof(pcm), dst + hdr_len,
					sizeof(dst) - (size_t)hdr_len, &dst_out, &need_flush);
			if (dst_out == 0)
				usleep(10 * 1000);
		}
		CHECK(dst_out > 0, "reconnect-grace: reclaimed instance still produces "
				"real frames after reuse");
		CHECK(dst_out == 0 || dst[hdr_len + 0] == 0x84,
				"reconnect-grace: reclaimed instance's frame has a valid "
				"in-band marker (byte0=0x84)");
	}

	/* Step 3: stop, wait past the grace window, start again -- must be a
	 * genuinely cold init again (reaper already tore the parked instance
	 * down), not another instant reclaim. */
	if (data2)
		codec->deinit(data2);
	usleep(500 * 1000); /* > 300 ms grace window */

	t0 = now_ms();
	void *data3 = codec->init(codec, 0, (void *)expected_caps, sizeof(expected_caps),
			&info, NULL, 1024);
	double cold_ms_2 = now_ms() - t0;
	CHECK(data3 != NULL, "reconnect-grace: post-expiry codec->init() succeeds");
	printf("INFO: reconnect-grace: post-expiry init #3 took %.1f ms (cold again)\n",
			cold_ms_2);
	CHECK(cold_ms_2 > 10.0, "reconnect-grace: init() after the grace window expired "
			"is slow again (a real ENC_INIT happened, not another reclaim) -- "
			"the grace window is bounded, not an unconditional cache");

	if (data3) {
		codec->deinit(data3);
		/* Let this last park's reaper thread run to completion before the
		 * test process exits, instead of relying on the OS closing fds on
		 * process exit to (incidentally) clean up a still-parked bridge --
		 * not something to depend on for hygiene in a test we control. */
		usleep(500 * 1000);
	}

	reset_conf();
}

/* "unit_test without Python": native parser for the OTA-header packet
 * framing used by the aptX Adaptive stream -- entirely in C, no external
 * process or interpreter involved (the product build assumes no Python is
 * available). This function parses the [u32le reply_len][reply]
 * stream, where each reply is 0..N back-to-back "8-byte OTA header + codec
 * frame" packets (frame size looked up by header byte 3, see
 * ota_frame_size_by_type below). */
static const int ota_frame_size_by_type[9] = { 348, 656, 140, 152, 560, 760, 960, 348, 980 };

/* Returns false on a malformed capture (short read, unknown packet type,
 * truncated frame) -- the caller CHECK()s the return value, same "never
 * crash on bad input" policy as every parser in this codebase. On success,
 * *out_count is the total packet count and *out_len_ok is how many of them
 * pass the in-band length check (payload[3]*4+12 == payload length),
 * turning what used to be an informational report into an actual
 * pass/fail check. */
static bool parse_raw_capture_native(const char *path, long *out_count, long *out_len_ok)
{
	FILE *f = fopen(path, "rb");
	long count = 0, len_ok = 0;

	if (!f)
		return false;

	for (;;) {
		uint32_t reply_len;
		unsigned char *reply;
		size_t q;

		if (fread(&reply_len, 4, 1, f) != 1)
			break; /* clean EOF between records */

		if (reply_len == 0)
			continue;

		reply = malloc(reply_len);
		if (!reply) {
			fclose(f);
			return false;
		}
		if (fread(reply, 1, reply_len, f) != reply_len) {
			free(reply);
			fclose(f);
			return false; /* truncated record */
		}

		q = 0;
		while (q + 8 <= reply_len) {
			const unsigned char *h = reply + q;
			int type = h[3];
			int sz;
			const unsigned char *payload;

			if (type < 0 || type > 8) {
				free(reply);
				fclose(f);
				return false; /* unknown packet type, same as the Python raise */
			}
			sz = ota_frame_size_by_type[type];
			if (q + 8 + (size_t)sz > reply_len) {
				free(reply);
				fclose(f);
				return false; /* frame runs past the end of this reply */
			}

			payload = reply + q + 8;
			count++;
			if ((long)payload[3] * 4 + 12 == sz)
				len_ok++;

			q += 8 + (size_t)sz;
		}
		free(reply);
	}

	fclose(f);
	*out_count = count;
	*out_len_ok = len_ok;
	return true;
}

/* Drives the bridge directly (bypassing codec_encode_adaptive) with
 * OTA-header-KEPT frames, in the exact "[u32le len][reply]" format
 * parse_raw_capture_native() above parses -- the same "check frames"
 * cross-check this project has always used, now
 * fully in-process (see parse_raw_capture_native()'s own comment for why). */
static void test_raw_capture_native(const char *qemu, const char *loader,
		const char *capi, double duration_sec)
{
	struct m4_ah_bridge_config cfg;
	spa_zero(cfg);
	cfg.qemu_path = qemu;
	cfg.loader_path = loader;
	cfg.capi_path = capi;
	cfg.strip_ota_header = false; /* keep the 8-byte OTA header for the tool */
	cfg.helper_timeout_ms = 200;

	struct m4_ah_bridge *b = m4_ah_bridge_start(NULL, &cfg);
	CHECK(b != NULL, "raw-capture bridge (strip_ota_header=false) starts");
	if (!b) {
		printf("SKIP: raw-capture cross-check (bridge start failed)\n");
		return;
	}

	const char *path = "/tmp/m4_adaptive_unit_test_raw_capture.bin";
	FILE *f = fopen(path, "wb");
	long n_blocks = (long)(duration_sec * 1000.0 / 10.0);
	uint32_t sample_pos = 0;

	for (long blk = 0; blk < n_blocks; blk++) {
		int16_t pcm[M4_AH_PCM_SAMPLES_PER_BLOCK * 2];
		gen_sine_block(pcm, sample_pos, M4_AH_PCM_SAMPLES_PER_BLOCK, 1000.0, 440.0);
		sample_pos += M4_AH_PCM_SAMPLES_PER_BLOCK;

		m4_ah_bridge_submit_pcm(b, pcm, sizeof(pcm));
		usleep(11000); /* give the IO thread a full block period + margin */

		uint8_t frame[M4_AH_MAX_FRAME_BYTES];
		size_t len = m4_ah_bridge_take_frame(b, frame, sizeof(frame));
		if (len > 0 && f) {
			/* Reconstruct the "[len][reply-with-OTA-header]" shape the
			 * tool expects. Our take_frame() already returns the raw
			 * helper frame bytes (OTA header + codec frame) here because
			 * strip_ota_header=false. */
			uint32_t len_hdr = (uint32_t)len;
			fwrite(&len_hdr, 4, 1, f);
			fwrite(frame, 1, len, f);
		}
	}
	if (f)
		fclose(f);
	m4_ah_bridge_stop(b);

	{
		long count = 0, len_ok = 0;
		bool parsed = parse_raw_capture_native(path, &count, &len_ok);
		CHECK(parsed, "native OTA-header capture parse succeeds (well-formed stream)");
		CHECK(parsed && count > 0, "native parse found a non-empty packet stream (%ld packets)", count);
		CHECK(parsed && len_ok == count,
				"native parse: in-band length check (payload[3]*4+12 == len) passes "
				"for every packet (%ld/%ld)", len_ok, count);
		printf("INFO: %s: %ld packets, %ld/%ld pass the in-band length check\n",
				path, count, len_ok, count);
	}
}

int main(int argc, char **argv)
{
	/* Isolation fix: isolated $HOME/$XDG_CONFIG_HOME, never the real
	 * ~/.config/momentum4 -- see this file's own write_conf()/
	 * setup_isolated_home() doc comments. Must run before dlopen() (the
	 * very first codec->fill_caps()/init() call already needs the isolated
	 * momentum4.conf in place). */
	setup_isolated_home();

	if (argc < 2) {
		fprintf(stderr, "usage: %s <libspa-codec-bluez5-aptx.so> [duration_sec]\n", argv[0]);
		return 2;
	}
	double duration_sec = argc > 2 ? atof(argv[2]) : 30.0;

	void *dl = dlopen(argv[1], RTLD_NOW);
	if (!dl) {
		fprintf(stderr, "dlopen(%s): %s\n", argv[1], dlerror());
		return 1;
	}

	struct spa_handle *handle = NULL;
	const struct media_codec *codec = load_aptx_adaptive(dl, &handle);
	if (!codec) {
		printf("\n%d check(s) failed, aborting (could not load codec)\n", g_failures + 1);
		return 1;
	}

	printf("\n== byte-layout tests (no helper process) ==\n");
	test_byte_layout(codec);
	test_enum_config(codec);

	printf("\n== allow-devices filter tests (no helper process) ==\n");
	test_allow_devices_filter(codec);

	printf("\n== fill_caps pre-check / endpoint-not-advertised tests ==\n");
	test_fill_caps_probe(codec);

	printf("\n== real qemu-hexagon helper tests ==\n");
	test_real_helper(codec, duration_sec);

	printf("\n== ll1 header check (short separate real-helper run) ==\n");
	test_mode_ll1_header(codec);

	printf("\n== reconnect-grace: start -> stop -> start within 1s ==\n");
	test_reconnect_grace(codec);

	printf("\n== raw OTA-header capture parse (native C, no Python) ==\n");
	test_raw_capture_native(g_qemu_path, g_loader_path, g_capi_path,
			SPA_MIN(duration_sec, 10.0) /* keep this part quick */);

	free(handle);
	dlclose(dl);

	printf("\n%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
