/* momentum4: unit test for the pure device-
 * identity/allow-list policy in m4-src/m4-device-policy.c -- shared by the
 * aptx_adaptive codec's allow-devices gate (patch 0009/0012/0014) and the
 * HFP call-control/sco-restore gates (patch 0006/0008/0013).
 *
 * Build/run (plain and ASan/UBSan):
 *   gcc -std=gnu11 -Wall -Wextra -Werror -Im4-src test/device_policy_test.c m4-src/m4-device-policy.c -o test/build/device_policy_test && test/build/device_policy_test
 *   gcc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -g \
 *       -Im4-src test/device_policy_test.c m4-src/m4-device-policy.c -o test/build-asan/device_policy_test_asan && test/build-asan/device_policy_test_asan
 */
#include <stdio.h>
#include <string.h>

#include "m4-device-policy.h"

static int checks, failures;

#define CHECK(c, msg)                                                             \
	do {                                                                      \
		checks++;                                                         \
		if (!(c)) {                                                       \
			failures++;                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
		}                                                                 \
	} while (0)

static void test_parse_allow_devices(void)
{
	char csv[M4_DEVICE_POLICY_MAC_CSV_MAX];
	bool valid;

	CHECK(m4_device_policy_parse_allow_devices(NULL, csv, sizeof csv, &valid) ==
			M4_DEVICE_POLICY_FINGERPRINT, "unset -> fingerprint");
	CHECK(valid, "unset is valid (not malformed, just default)");

	CHECK(m4_device_policy_parse_allow_devices("", csv, sizeof csv, NULL) ==
			M4_DEVICE_POLICY_FINGERPRINT, "empty -> fingerprint");

	CHECK(m4_device_policy_parse_allow_devices("any", csv, sizeof csv, &valid) ==
			M4_DEVICE_POLICY_ANY, "any -> ANY");
	CHECK(valid, "any is valid");
	CHECK(m4_device_policy_parse_allow_devices("ANY", csv, sizeof csv, NULL) ==
			M4_DEVICE_POLICY_ANY, "ANY case-insensitive");

	CHECK(m4_device_policy_parse_allow_devices("adk-stereo", csv, sizeof csv, &valid) ==
			M4_DEVICE_POLICY_FINGERPRINT, "adk-stereo -> FINGERPRINT");
	CHECK(valid, "adk-stereo is valid");
	CHECK(m4_device_policy_parse_allow_devices("m4", csv, sizeof csv, &valid) ==
			M4_DEVICE_POLICY_FINGERPRINT, "m4 alias -> FINGERPRINT");
	CHECK(valid, "m4 alias is valid");
	CHECK(m4_device_policy_parse_allow_devices("M4", csv, sizeof csv, NULL) ==
			M4_DEVICE_POLICY_FINGERPRINT, "M4 case-insensitive");

	CHECK(m4_device_policy_parse_allow_devices("00:1B:66:00:00:01", csv, sizeof csv, &valid) ==
			M4_DEVICE_POLICY_MAC_LIST, "single MAC -> MAC_LIST");
	CHECK(valid, "single MAC is valid");
	CHECK(!strcmp(csv, "00:1B:66:00:00:01"), "csv copied verbatim");

	CHECK(m4_device_policy_parse_allow_devices(
			"00:1B:66:00:00:01, 11:22:33:44:55:66 ,AA:BB:CC:DD:EE:FF",
			csv, sizeof csv, &valid) == M4_DEVICE_POLICY_MAC_LIST, "3-MAC csv -> MAC_LIST");
	CHECK(valid, "3-MAC csv is valid");

	CHECK(m4_device_policy_parse_allow_devices("garbage", csv, sizeof csv, &valid) ==
			M4_DEVICE_POLICY_FINGERPRINT, "garbage -> falls back to FINGERPRINT");
	CHECK(!valid, "garbage is flagged invalid");

	CHECK(m4_device_policy_parse_allow_devices("00:1B:66:00:00", csv, sizeof csv, &valid) ==
			M4_DEVICE_POLICY_FINGERPRINT, "truncated MAC -> fallback FINGERPRINT");
	CHECK(!valid, "truncated MAC is invalid");

	CHECK(m4_device_policy_parse_allow_devices("00:1B:66:00:00:01,garbage", csv, sizeof csv, &valid) ==
			M4_DEVICE_POLICY_FINGERPRINT, "one bad token in csv -> whole list rejected, fallback FINGERPRINT");
	CHECK(!valid, "partially-bad csv is invalid");
}

static void test_mac_list_contains(void)
{
	const char *csv = "00:1B:66:00:00:01, 11:22:33:44:55:66";

	CHECK(m4_device_policy_mac_list_contains(csv, "00:1B:66:00:00:01"), "exact match");
	CHECK(m4_device_policy_mac_list_contains(csv, "00:1b:66:00:00:01"), "case-insensitive");
	CHECK(m4_device_policy_mac_list_contains(csv, "11:22:33:44:55:66"), "second entry (whitespace-trimmed)");
	CHECK(!m4_device_policy_mac_list_contains(csv, "00:00:00:00:00:00"), "not in list");
	CHECK(!m4_device_policy_mac_list_contains(csv, NULL), "NULL address");
	CHECK(!m4_device_policy_mac_list_contains(NULL, "00:1B:66:00:00:01"), "NULL csv");
	CHECK(!m4_device_policy_mac_list_contains("", "00:1B:66:00:00:01"), "empty csv");
}

static void test_parse_hfp_mode(void)
{
	CHECK(m4_device_policy_parse_hfp_mode(NULL) == M4_DEVICE_POLICY_IDENTITY, "unset -> identity");
	CHECK(m4_device_policy_parse_hfp_mode("") == M4_DEVICE_POLICY_IDENTITY, "empty -> identity");
	CHECK(m4_device_policy_parse_hfp_mode("m4") == M4_DEVICE_POLICY_IDENTITY, "m4 -> identity");
	CHECK(m4_device_policy_parse_hfp_mode("M4") == M4_DEVICE_POLICY_IDENTITY, "M4 case-insensitive");
	CHECK(m4_device_policy_parse_hfp_mode("all") == M4_DEVICE_POLICY_ANY, "all -> any");
	CHECK(m4_device_policy_parse_hfp_mode("ALL") == M4_DEVICE_POLICY_ANY, "ALL case-insensitive");
	CHECK(m4_device_policy_parse_hfp_mode("off") == M4_DEVICE_POLICY_OFF, "off -> off");
	CHECK(m4_device_policy_parse_hfp_mode("OFF") == M4_DEVICE_POLICY_OFF, "OFF case-insensitive");
	CHECK(m4_device_policy_parse_hfp_mode("garbage") == M4_DEVICE_POLICY_IDENTITY, "garbage -> identity (safe default)");
}

static void test_identity_matches(void)
{
	CHECK(m4_device_policy_identity_matches("MOMENTUM 4", "00:1B:66:00:00:01", "MOMENTUM 4", "auto"),
			"exact name match");
	CHECK(m4_device_policy_identity_matches("Momentum 4 (left)", "00:1B:66:00:00:01", "MOMENTUM 4", "auto"),
			"prefix, case-insensitive");
	CHECK(!m4_device_policy_identity_matches("PRO X", "11:22:33:44:55:66", "MOMENTUM 4", "auto"),
			"other headset, no match");
	CHECK(!m4_device_policy_identity_matches("MOMENTUM", "11:22:33:44:55:66", "MOMENTUM 4", "auto"),
			"short prefix does not match");
	CHECK(!m4_device_policy_identity_matches(NULL, NULL, "MOMENTUM 4", "auto"), "NULL name/address, auto address");

	/* address-only match (name does not match, but the configured [device]
	 * address is an exact MAC and matches -- "identity fingerprint, and
	 * optionally MAC", OR semantics). */
	CHECK(m4_device_policy_identity_matches("Some Other Name", "00:1B:66:00:00:01",
			"MOMENTUM 4", "00:1B:66:00:00:01"), "address-only match");
	CHECK(!m4_device_policy_identity_matches("Some Other Name", "11:22:33:44:55:66",
			"MOMENTUM 4", "00:1B:66:00:00:01"), "address configured but does not match, name does not either");

	/* "auto"/empty configured_address never itself matches. */
	CHECK(!m4_device_policy_identity_matches("Other", "auto", "MOMENTUM 4", "auto"),
			"literal address=\"auto\" is not a real address, no false match");
	CHECK(!m4_device_policy_identity_matches("Other", "00:1B:66:00:00:01", "MOMENTUM 4", ""),
			"empty configured_address never matches");
}

int main(void)
{
	test_parse_allow_devices();
	test_mac_list_contains();
	test_parse_hfp_mode();
	test_identity_matches();
	printf("device_policy_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
