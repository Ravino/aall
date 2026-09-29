#include "profile_rules.h"

#include "../../../common/m4-i18n.h"

#include <string.h>
#include <strings.h> /* strcasecmp */

enum m4_mode m4_profile_mode(const char *profile_name)
{
	if (profile_name == NULL || profile_name[0] == '\0')
		return M4_MODE_UNKNOWN;
	if (strcmp(profile_name, "off") == 0)
		return M4_MODE_OFF;
	if (strcmp(profile_name, "headset-head-unit-cvsd") == 0)
		return M4_MODE_HFP_CVSD;
	if (strcmp(profile_name, "headset-head-unit") == 0)
		return M4_MODE_HFP_MSBC;
	/* "a2dp-sink" (the auto profile actually used) and "a2dp-sink-<codec>"
	 * (explicit per-codec profiles, only ever seen in EnumProfile) both
	 * name the A2DP music path -- see header comment. */
	if (strncmp(profile_name, "a2dp-sink", 9) == 0)
		return M4_MODE_AALL;
	return M4_MODE_UNKNOWN;
}

const char *m4_mode_token(enum m4_mode mode)
{
	switch (mode) {
	case M4_MODE_OFF: return "off";
	case M4_MODE_AALL: return "aall";
	case M4_MODE_HFP_MSBC: return "hfp_msbc";
	case M4_MODE_HFP_CVSD: return "hfp_cvsd";
	case M4_MODE_UNKNOWN: default: return "unknown";
	}
}

const char *m4_mode_human(enum m4_mode mode)
{
	switch (mode) {
	case M4_MODE_OFF: return M4_T("off", "выключен");
	case M4_MODE_AALL: return "AALL";
	case M4_MODE_HFP_MSBC: return "HFP (mSBC)";
	case M4_MODE_HFP_CVSD: return "HFP (CVSD)";
	case M4_MODE_UNKNOWN: default: return M4_T("unknown", "неизвестно");
	}
}

int m4_bt_address_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcasecmp(a, b) == 0;
}
