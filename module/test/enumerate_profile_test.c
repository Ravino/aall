/* momentum4 diagnostics helper: reproduces, OFFLINE and
 * with NO D-Bus/BlueZ/live PipeWire involved, the exact index-stepping
 * algorithm that bluez5-device.c::get_profile_from_index() uses to build
 * SPA_PARAM_EnumProfile (see impl_enum_params() case SPA_PARAM_EnumProfile,
 * which literally does `result.index = result.next++;` then calls
 * get_profile_from_index(this, result.index, &result.next, &codec)`).
 *
 * get_profile_from_index() itself is `static` inside bluez5-device.c, so it
 * cannot be linked/dlopen()'d directly for a unit test the way codec-loader.c
 * functions are (see codec_order_test.c's own comment on this). Instead this
 * test includes the REAL project headers (spa/param/bluetooth/audio.h for
 * the enum, spa/param/bluetooth/type-info.h for spa_type_bluetooth_audio_codec[])
 * unmodified, and reimplements ONLY the A2DP-branch stepping logic verbatim
 * from bluez5-device.c (kept in sync manually; the line range is cited below
 * so a future diff against the real file is a one-line grep away).
 *
 * Usage: enumerate_profile_test
 * Exit code 0 = aptx_adaptive (id 20) IS reachable by the EnumProfile walk.
 * Exit code 1 = it is NOT reachable (the bug this test was written to catch).
 */
#include <stdio.h>
#include <stdint.h>

#include <spa/utils/defs.h>
#include <spa/utils/type.h>
#include <spa/param/bluetooth/audio.h>
#include <spa/param/bluetooth/type-info.h>

/* Mirrors bluez5-device.c's DEVICE_PROFILE_* enum (bluez5-device.c:57-62). */
enum {
	DEVICE_PROFILE_OFF = 0,
	DEVICE_PROFILE_AG = 1,
	DEVICE_PROFILE_A2DP = 2,
	DEVICE_PROFILE_HSP_HFP = 3,
	DEVICE_PROFILE_BAP = 4,
	DEVICE_PROFILE_ASHA = 5,
};

/* Verbatim copy of the A2DP/BAP/HSP_HFP branch of
 * bluez5-device.c:get_profile_from_index() (lines 1639-1648 as of this
 * writing). Only this branch matters for the aptx_adaptive bug. */
static uint32_t get_profile_from_index_a2dp_branch(uint32_t index, uint32_t *next,
		enum spa_bluetooth_audio_codec *codec)
{
	uint32_t profile = (index >> 16);
	const struct spa_type_info *info;

	*codec = (index & 0xffff);
	*next = (profile + 1) << 16;

	for (info = spa_type_bluetooth_audio_codec; info->type; ++info)
		if (info->type > (uint32_t)*codec)
			*next = SPA_MIN(*next, (profile << 16) | (info->type & 0xffff));
	return profile;
}

int main(void)
{
	uint32_t index = (DEVICE_PROFILE_A2DP << 16) | 0;
	uint32_t next;
	enum spa_bluetooth_audio_codec codec;
	int seen_target = 0, steps = 0;
	const uint32_t target = SPA_BLUETOOTH_AUDIO_CODEC_APTX_ADAPTIVE_M4_EXPERIMENTAL;

	printf("Walking A2DP index space via the real spa_type_bluetooth_audio_codec[]"
			" table (as SPA_PARAM_EnumProfile does)...\n");
	while (steps++ < 1000) {
		uint32_t p = get_profile_from_index_a2dp_branch(index, &next, &codec);
		if ((next >> 16) != DEVICE_PROFILE_A2DP && !seen_target) {
			/* one more print of the boundary step, then stop */
			printf("  index=0x%08x -> profile=%u codec=%u next=0x%08x (leaving A2DP)\n",
					index, p, (unsigned)codec, next);
		} else {
			printf("  index=0x%08x -> profile=%u codec=%u next=0x%08x\n",
					index, p, (unsigned)codec, next);
		}
		if ((uint32_t)codec == target)
			seen_target = 1;
		if (next == index)
			break; /* safety against infinite loop */
		index = next;
		if ((index >> 16) != DEVICE_PROFILE_A2DP)
			break;
	}

	printf("\nRESULT: SPA_BLUETOOTH_AUDIO_CODEC_APTX_ADAPTIVE_M4_EXPERIMENTAL (id=%u) %s"
			" reachable by SPA_PARAM_EnumProfile's index walk.\n",
			target, seen_target ? "IS" : "is NEVER");

	return seen_target ? 0 : 1;
}
