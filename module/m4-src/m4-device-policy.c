/* momentum4: implementation, see m4-device-policy.h.
 * SPDX-License-Identifier: MIT
 */
#include "m4-device-policy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp, strncasecmp */

static void trim(const char *start, const char *end, const char **out_start, const char **out_end)
{
	while (start < end && isspace((unsigned char)*start))
		start++;
	while (end > start && isspace((unsigned char)end[-1]))
		end--;
	*out_start = start;
	*out_end = end;
}

/* Exactly "XX:XX:XX:XX:XX:XX", 6 hex octets, ':'-separated, case-insensitive. */
static bool looks_like_one_mac(const char *s, size_t len)
{
	if (len != 17)
		return false;
	for (size_t i = 0; i < len; i++) {
		if (i % 3 == 2) {
			if (s[i] != ':')
				return false;
		} else if (!isxdigit((unsigned char)s[i])) {
			return false;
		}
	}
	return true;
}

static bool csv_looks_like_mac_list(const char *value)
{
	const char *p = value;

	if (value == NULL || *value == '\0')
		return false;

	while (*p != '\0') {
		const char *comma = strchr(p, ',');
		const char *tok_end = comma ? comma : p + strlen(p);
		const char *ts, *te;

		trim(p, tok_end, &ts, &te);
		if (te <= ts || !looks_like_one_mac(ts, (size_t)(te - ts)))
			return false;

		if (!comma)
			break;
		p = comma + 1;
	}
	return true;
}

enum m4_device_policy_mode m4_device_policy_parse_allow_devices(const char *value,
		char *mac_csv_out, size_t mac_csv_cap, bool *out_valid)
{
	if (out_valid)
		*out_valid = true;
	if (mac_csv_out && mac_csv_cap > 0)
		mac_csv_out[0] = '\0';

	if (value == NULL || *value == '\0')
		return M4_DEVICE_POLICY_FINGERPRINT;

	if (!strcasecmp(value, "any"))
		return M4_DEVICE_POLICY_ANY;
	if (!strcasecmp(value, "adk-stereo") || !strcasecmp(value, "m4"))
		return M4_DEVICE_POLICY_FINGERPRINT;

	if (csv_looks_like_mac_list(value)) {
		if (mac_csv_out && mac_csv_cap > 0)
			snprintf(mac_csv_out, mac_csv_cap, "%s", value);
		return M4_DEVICE_POLICY_MAC_LIST;
	}

	if (out_valid)
		*out_valid = false;
	return M4_DEVICE_POLICY_FINGERPRINT;
}

enum m4_device_policy_mode m4_device_policy_parse_hfp_mode(const char *value)
{
	if (value == NULL || *value == '\0')
		return M4_DEVICE_POLICY_IDENTITY;
	if (!strcasecmp(value, "off"))
		return M4_DEVICE_POLICY_OFF;
	if (!strcasecmp(value, "all"))
		return M4_DEVICE_POLICY_ANY;
	return M4_DEVICE_POLICY_IDENTITY; /* "m4" or anything else */
}

bool m4_device_policy_mac_list_contains(const char *mac_csv, const char *address)
{
	const char *p = mac_csv;
	size_t addr_len;

	if (mac_csv == NULL || *mac_csv == '\0' || address == NULL || *address == '\0')
		return false;
	addr_len = strlen(address);

	while (*p != '\0') {
		const char *comma = strchr(p, ',');
		const char *tok_end = comma ? comma : p + strlen(p);
		const char *ts, *te;

		trim(p, tok_end, &ts, &te);
		if (te > ts && (size_t)(te - ts) == addr_len &&
				strncasecmp(ts, address, addr_len) == 0)
			return true;

		if (!comma)
			break;
		p = comma + 1;
	}
	return false;
}

bool m4_device_policy_identity_matches(const char *name, const char *address,
		const char *name_match_prefix, const char *configured_address)
{
	if (name != NULL && name_match_prefix != NULL && *name_match_prefix != '\0') {
		size_t prefix_len = strlen(name_match_prefix);
		if (strncasecmp(name, name_match_prefix, prefix_len) == 0)
			return true;
	}
	if (address != NULL && configured_address != NULL && *configured_address != '\0' &&
			strcasecmp(configured_address, "auto") != 0) {
		if (strcasecmp(address, configured_address) == 0)
			return true;
	}
	return false;
}
