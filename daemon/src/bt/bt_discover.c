/* bt/bt_discover.c -- see bt_discover.h. */
#include "bt_discover.h"

#include "../../../common/m4-i18n.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <systemd/sd-bus.h>

struct candidate {
	char address[32];
	char name[256];
	char alias[256];
	char adapter[64]; /* from the Adapter object-path property, bare "hciN" */
	bool paired;
	bool is_device;
};

static const char *adapter_basename(const char *object_path)
{
	const char *slash = strrchr(object_path, '/');

	return slash != NULL ? slash + 1 : object_path;
}

static bool name_has_prefix(const char *name, const char *prefix)
{
	size_t n;

	if (name == NULL || name[0] == '\0' || prefix == NULL || prefix[0] == '\0')
		return false;
	n = strlen(prefix);
	if (strlen(name) < n)
		return false;
	return strncasecmp(name, prefix, n) == 0;
}

/* Reads one "sv" (key, variant) dict entry already entered by the caller
 * (sd_bus_message_enter_container(SD_BUS_TYPE_DICT_ENTRY, "sv")) into
 * `cand`, recognising only the Device1 properties this file cares about;
 * anything else is skipped whole via sd_bus_message_skip(). Returns <0 on a
 * real parse error, 0 otherwise (including "recognised but this property
 * has an unexpected variant signature", which is treated as absent rather
 * than fatal -- a BlueZ version mismatch here must never crash momentum4d). */
static int read_device_property(sd_bus_message *m, struct candidate *cand)
{
	const char *key = NULL;
	const char *contents = NULL;
	int r;

	r = sd_bus_message_read(m, "s", &key);
	if (r < 0)
		return r;

	r = sd_bus_message_peek_type(m, NULL, &contents);
	if (r < 0)
		return r;

	if (key != NULL && contents != NULL && strcmp(key, "Address") == 0 && strcmp(contents, "s") == 0) {
		const char *v = NULL;

		r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s");
		if (r < 0)
			return r;
		r = sd_bus_message_read(m, "s", &v);
		if (r < 0)
			return r;
		if (v != NULL)
			snprintf(cand->address, sizeof(cand->address), "%s", v);
		return sd_bus_message_exit_container(m);
	}
	if (key != NULL && contents != NULL && strcmp(key, "Name") == 0 && strcmp(contents, "s") == 0) {
		const char *v = NULL;

		r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s");
		if (r < 0)
			return r;
		r = sd_bus_message_read(m, "s", &v);
		if (r < 0)
			return r;
		if (v != NULL)
			snprintf(cand->name, sizeof(cand->name), "%s", v);
		return sd_bus_message_exit_container(m);
	}
	if (key != NULL && contents != NULL && strcmp(key, "Alias") == 0 && strcmp(contents, "s") == 0) {
		const char *v = NULL;

		r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "s");
		if (r < 0)
			return r;
		r = sd_bus_message_read(m, "s", &v);
		if (r < 0)
			return r;
		if (v != NULL)
			snprintf(cand->alias, sizeof(cand->alias), "%s", v);
		return sd_bus_message_exit_container(m);
	}
	if (key != NULL && contents != NULL && strcmp(key, "Paired") == 0 && strcmp(contents, "b") == 0) {
		int v = 0;

		r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "b");
		if (r < 0)
			return r;
		r = sd_bus_message_read(m, "b", &v);
		if (r < 0)
			return r;
		cand->paired = v != 0;
		return sd_bus_message_exit_container(m);
	}
	if (key != NULL && contents != NULL && strcmp(key, "Adapter") == 0 && strcmp(contents, "o") == 0) {
		const char *v = NULL;

		r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "o");
		if (r < 0)
			return r;
		r = sd_bus_message_read(m, "o", &v);
		if (r < 0)
			return r;
		if (v != NULL)
			snprintf(cand->adapter, sizeof(cand->adapter), "%s", adapter_basename(v));
		return sd_bus_message_exit_container(m);
	}

	/* Unrecognised property (or a recognised key with an unexpected
	 * variant signature, e.g. a future BlueZ making Paired a variant of
	 * something else) -- skip the whole variant payload, whatever shape
	 * it is. */
	return sd_bus_message_skip(m, "v");
}

/* Reads one "oa{sa{sv}}" dict entry (one managed object) already entered by
 * the caller. Fills `cand` and sets cand->is_device if this object exposes
 * org.bluez.Device1. `object_path` (borrowed, valid only during this call)
 * is used purely as a last-resort adapter-name fallback if the Adapter
 * property itself was absent. */
static int read_managed_object(sd_bus_message *m, struct candidate *cand)
{
	const char *object_path = NULL;
	int r;

	memset(cand, 0, sizeof(*cand));

	r = sd_bus_message_read(m, "o", &object_path);
	if (r < 0)
		return r;

	r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sa{sv}}");
	if (r < 0)
		return r;

	while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}")) > 0) {
		const char *iface = NULL;

		r = sd_bus_message_read(m, "s", &iface);
		if (r < 0)
			return r;
		if (iface != NULL && strcmp(iface, "org.bluez.Device1") == 0)
			cand->is_device = true;

		r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
		if (r < 0)
			return r;
		while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
			r = read_device_property(m, cand);
			if (r < 0)
				return r;
			r = sd_bus_message_exit_container(m); /* dict entry "sv" */
			if (r < 0)
				return r;
		}
		if (r < 0)
			return r;
		r = sd_bus_message_exit_container(m); /* array "{sv}" */
		if (r < 0)
			return r;
		r = sd_bus_message_exit_container(m); /* dict entry "sa{sv}" */
		if (r < 0)
			return r;
	}
	if (r < 0)
		return r;
	r = sd_bus_message_exit_container(m); /* array "{sa{sv}}" */
	if (r < 0)
		return r;

	if (cand->is_device && cand->adapter[0] == '\0' && object_path != NULL) {
		/* Adapter property absent (should not happen for a real Device1,
		 * but never trust a remote daemon blindly): "/org/bluez/hciN/
		 * dev_XX_XX..." -- the adapter is the SECOND path component. */
		char tmp[192];
		char *second_slash;

		snprintf(tmp, sizeof(tmp), "%s", object_path);
		if (tmp[0] == '/') {
			second_slash = strchr(tmp + 1, '/');
			if (second_slash != NULL) {
				char *third_slash = strchr(second_slash + 1, '/');

				if (third_slash != NULL)
					*third_slash = '\0';
				snprintf(cand->adapter, sizeof(cand->adapter), "%s", second_slash + 1);
			}
		}
	}
	return 0;
}

int m4_bt_discover_auto(const char *name_match, char *out_mac, size_t out_mac_cap, char *out_adapter,
			 size_t out_adapter_cap, char *err_buf, size_t err_buf_cap)
{
	sd_bus *bus = NULL;
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int r;
	int rc = -1;

	if (err_buf != NULL && err_buf_cap > 0)
		err_buf[0] = '\0';
	if (out_mac != NULL && out_mac_cap > 0)
		out_mac[0] = '\0';
	if (out_adapter != NULL && out_adapter_cap > 0)
		out_adapter[0] = '\0';

	if (name_match == NULL || name_match[0] == '\0') {
		if (err_buf != NULL)
			snprintf(err_buf, err_buf_cap, M4_T("name-match is empty -- auto-discovery disabled",
"name-match пуст -- auto-обнаружение отключено"));
		return -1;
	}

	r = sd_bus_open_system(&bus);
	if (r < 0) {
		if (err_buf != NULL)
			snprintf(err_buf, err_buf_cap, "sd_bus_open_system: %s", strerror(-r));
		return -1;
	}

	r = sd_bus_call_method(bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
				"GetManagedObjects", &error, &reply, "");
	if (r < 0) {
		if (err_buf != NULL)
			snprintf(err_buf, err_buf_cap, "GetManagedObjects: %s",
				 error.message != NULL ? error.message : strerror(-r));
		goto out;
	}

	r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}");
	if (r < 0) {
		if (err_buf != NULL)
			snprintf(err_buf, err_buf_cap, M4_T("GetManagedObjects reply: unexpected shape",
"ответ GetManagedObjects: неожиданный формат"));
		goto out;
	}

	while ((r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}")) > 0) {
		struct candidate cand;
		int rr = read_managed_object(reply, &cand);

		if (rr < 0) {
			r = rr;
			break;
		}
		r = sd_bus_message_exit_container(reply); /* dict entry "oa{sa{sv}}" */
		if (r < 0)
			break;

		if (rc != 0 && cand.is_device && cand.paired && cand.address[0] != '\0') {
			const char *display = cand.alias[0] != '\0' ? cand.alias : cand.name;

			if (name_has_prefix(display, name_match)) {
				if (out_mac != NULL)
					snprintf(out_mac, out_mac_cap, "%s", cand.address);
				if (out_adapter != NULL && cand.adapter[0] != '\0')
					snprintf(out_adapter, out_adapter_cap, "%s", cand.adapter);
				rc = 0;
				/* Keep draining the reply (r stays > 0 while more dict
				 * entries remain) instead of an early return -- an
				 * sd_bus_message with unread containers must still be
				 * unref'd cleanly below, and doing that mid-iteration
				 * would leave the container stack unbalanced. */
			}
		}
	}
	if (r < 0 && err_buf != NULL && err_buf[0] == '\0')
		snprintf(err_buf, err_buf_cap, M4_T("GetManagedObjects reply: parse error (%s)",
"ответ GetManagedObjects: ошибка разбора (%s)"), strerror(-r));

	if (rc != 0 && err_buf != NULL && err_buf[0] == '\0')
		snprintf(err_buf, err_buf_cap, M4_T("no paired device matched name-match=\"%s\"",
"ни одно сопряжённое устройство не совпало с name-match=\"%s\""),
			 name_match);

out:
	sd_bus_error_free(&error);
	if (reply != NULL)
		sd_bus_message_unref(reply);
	if (bus != NULL)
		sd_bus_unref(bus);
	return rc;
}
