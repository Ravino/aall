/* momentum4: verifies that aptx_adaptive actually sorts FIRST out of
 * the whole codec set once loaded the exact way bluez5-dbus.c does it --
 * i.e. through the real codec-loader.c::load_media_codecs(), not just by
 * eyeballing the priority table in codec-loader.c's codec_order() (which is
 * `static`, not directly unit-testable in isolation).
 *
 * Implements a minimal struct spa_plugin_loader whose load() just dlopen()s
 * "<dir>/<SPA_KEY_LIBRARY_NAME>.so" and returns its (only) exported handle --
 * a simplified stand-in for what pw_context's real plugin loader does
 * (id-based factory lookup is skipped since every one of our .so files here
 * exports exactly one factory).
 *
 * Usage: codec_order_test <build-dir>/spa/plugins/bluez5
 * (i.e. the directory containing libspa-bluez5.so, libspa-codec-bluez5-aptx.so,
 * -sbc.so, -faststream.so from `ninja -C build spa/plugins/bluez5/...`).
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/support/log.h>
#include <spa/support/plugin.h>
#include <spa/support/plugin-loader.h>
#include <spa/utils/dict.h>

#include "codec-loader.h"

static int g_failures = 0;
#define CHECK(cond, ...) do { \
	if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
	else { printf("PASS: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static const char *g_dir;

static struct spa_handle *fake_load(void *object, const char *factory_name,
		const struct spa_dict *info)
{
	const char *libname = info ? spa_dict_lookup(info, SPA_KEY_LIBRARY_NAME) : NULL;
	char path[1024];
	void *dl;
	typedef int (*enum_fn_t)(const struct spa_handle_factory **factory, uint32_t *index);
	enum_fn_t enum_fn;
	const struct spa_handle_factory *factory = NULL;
	uint32_t idx = 0;
	struct spa_handle *handle;

	(void)object; (void)factory_name;
	if (!libname)
		return NULL;

	snprintf(path, sizeof(path), "%s/%s.so", g_dir, libname);
	dl = dlopen(path, RTLD_NOW);
	if (getenv("CODEC_ORDER_TEST_DEBUG"))
		fprintf(stderr, "DEBUG: dlopen(%s) -> %p (%s)\n", path, dl, dl ? "ok" : dlerror());
	if (!dl) {
		/* Same as the real loader: missing plugin is not fatal, that codec
		 * (or, for a real system, this fallback to the next SPA_PLUGIN_DIR
		 * entry) is just skipped -- see codec-loader.c's
		 * load_media_codecs_from(). Our isolated build does not enable
		 * ldac/aac/opus/lc3(plus)/g722,
		 * so this is expected and not a test failure by itself. */
		return NULL;
	}
	enum_fn = (enum_fn_t)dlsym(dl, "spa_handle_factory_enum");
	if (!enum_fn || enum_fn(&factory, &idx) != 1 || !factory)
		return NULL;

	handle = calloc(1, factory->get_size(factory, NULL));
	if (!handle)
		return NULL;
	if (factory->init(factory, handle, NULL, NULL, 0) < 0) {
		free(handle);
		return NULL;
	}
	return handle;
}

static int fake_unload(void *object, struct spa_handle *handle)
{
	(void)object;
	free(handle);
	return 0;
}

static const struct spa_plugin_loader_methods fake_loader_methods = {
	SPA_VERSION_PLUGIN_LOADER_METHODS,
	.load = fake_load,
	.unload = fake_unload,
};

int main(int argc, char **argv)
{
	struct spa_plugin_loader loader;
	const struct media_codec * const *codecs;
	int aptx_adaptive_idx = -1, aptx_hd_idx = -1, sbc_idx = -1, n = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <build-dir>/spa/plugins/bluez5\n", argv[0]);
		return 2;
	}
	g_dir = argv[1];

	loader.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_PluginLoader,
			SPA_VERSION_PLUGIN_LOADER, &fake_loader_methods, NULL);

	/* NULL log is fine: every spa_log_* macro this exercises is guarded by
	 * spa_log_level_topic_enabled()/spa_log_topic_init() checking for a NULL
	 * log first (spa/include/spa/support/log.h) -- confirmed by reading
	 * those inline functions, not just assumed. */
	codecs = load_media_codecs(&loader, NULL);
	CHECK(codecs != NULL, "load_media_codecs() succeeds against the real "
			"codec-loader.c + our built .so files");
	if (!codecs)
		return 1;

	printf("INFO: codec priority order as loaded:\n");
	for (n = 0; codecs[n]; n++) {
		printf("  [%d] %s\n", n, codecs[n]->name);
		if (!strcmp(codecs[n]->name, "aptx_adaptive")) aptx_adaptive_idx = n;
		if (!strcmp(codecs[n]->name, "aptx_hd")) aptx_hd_idx = n;
		if (!strcmp(codecs[n]->name, "sbc")) sbc_idx = n;
	}

	CHECK(aptx_adaptive_idx == 0, "aptx_adaptive is FIRST in the loaded/sorted "
			"codec list (requirement: \"aptx_adaptive first for the M4\") (got index %d)",
			aptx_adaptive_idx);
	CHECK(sbc_idx >= 0, "sbc is present (mandatory fallback codec, "
			"load_media_codecs() itself requires this)");
	CHECK(aptx_hd_idx < 0 || aptx_adaptive_idx < aptx_hd_idx,
			"aptx_adaptive sorts before aptx_hd (idx %d < %d)",
			aptx_adaptive_idx, aptx_hd_idx);
	CHECK(sbc_idx < 0 || aptx_adaptive_idx < sbc_idx,
			"aptx_adaptive sorts before sbc (idx %d < %d)", aptx_adaptive_idx, sbc_idx);

	free_media_codecs(codecs);

	printf("\n%d check(s) failed\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
