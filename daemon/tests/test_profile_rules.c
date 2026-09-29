/* Unit tests for core/profile_rules.c -- pure functions, no I/O. */
#include <stdio.h>
#include <string.h>

#include "core/profile_rules.h"

static int failures = 0;

#define CHECK(cond, msg) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
			failures++; \
		} \
	} while (0)

static void test_mode_mapping(void)
{
	CHECK(m4_profile_mode("off") == M4_MODE_OFF, "off");
	CHECK(m4_profile_mode("headset-head-unit") == M4_MODE_HFP_MSBC, "headset-head-unit");
	CHECK(m4_profile_mode("headset-head-unit-cvsd") == M4_MODE_HFP_CVSD, "headset-head-unit-cvsd");
	CHECK(m4_profile_mode("a2dp-sink") == M4_MODE_AALL, "a2dp-sink");
	CHECK(m4_profile_mode("a2dp-sink-aptx_hd") == M4_MODE_AALL, "a2dp-sink-aptx_hd");
	CHECK(m4_profile_mode("a2dp-sink-sbc_xq") == M4_MODE_AALL, "a2dp-sink-sbc_xq");
	CHECK(m4_profile_mode("pro-audio") == M4_MODE_UNKNOWN, "pro-audio (unrelated profile name)");
	CHECK(m4_profile_mode("") == M4_MODE_UNKNOWN, "empty string");
	CHECK(m4_profile_mode(NULL) == M4_MODE_UNKNOWN, "NULL");
	/* must not confuse the mSBC and CVSD variants with each other via a
	 * naive prefix match ("headset-head-unit" is a prefix of
	 * "headset-head-unit-cvsd") */
	CHECK(m4_profile_mode("headset-head-unit-cvsd") != M4_MODE_HFP_MSBC,
	      "cvsd must not be reported as msbc");
}

static void test_tokens_nonempty(void)
{
	enum m4_mode modes[] = {
		M4_MODE_UNKNOWN, M4_MODE_OFF, M4_MODE_AALL, M4_MODE_HFP_MSBC, M4_MODE_HFP_CVSD,
	};
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		CHECK(m4_mode_token(modes[i])[0] != '\0', "m4_mode_token must never be empty");
		CHECK(m4_mode_human(modes[i])[0] != '\0', "m4_mode_human must never be empty");
	}
	CHECK(strcmp(m4_mode_token(M4_MODE_AALL), "aall") == 0, "aall token");
	CHECK(strcmp(m4_mode_token(M4_MODE_HFP_MSBC), "hfp_msbc") == 0, "hfp_msbc token");
	CHECK(strcmp(m4_mode_token(M4_MODE_HFP_CVSD), "hfp_cvsd") == 0, "hfp_cvsd token");
}

static void test_bt_address_equal(void)
{
	CHECK(m4_bt_address_equal("00:1B:66:00:00:01", "00:1B:66:00:00:01"), "identical");
	CHECK(m4_bt_address_equal("00:1b:66:00:00:01", "00:1B:66:00:00:01"),
	      "case-insensitive (BlueZ/PipeWire always upper-case, but must not rely on that)");
	CHECK(!m4_bt_address_equal("00:1B:66:00:00:01", "AA:BB:CC:DD:EE:FF"), "different addresses");
	CHECK(m4_bt_address_equal(NULL, NULL), "NULL == NULL");
	CHECK(!m4_bt_address_equal(NULL, "00:1B:66:00:00:01"), "NULL != non-NULL");
	CHECK(!m4_bt_address_equal("00:1B:66:00:00:01", NULL), "non-NULL != NULL");
}

int main(void)
{
	test_mode_mapping();
	test_tokens_nonempty();
	test_bt_address_equal();

	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("profile_rules: all checks passed\n");
	return 0;
}
