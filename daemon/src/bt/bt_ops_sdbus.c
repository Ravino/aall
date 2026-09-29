/* bt/bt_ops_sdbus.c -- real m4_bt_ops implementation, sd-bus/system bus.
 * org.bluez.Device1.ConnectProfile() error shapes (confirmed by reading
 * work/src/bluez-5.66/src/device.c's connect_profiles()/error.c): a
 * profile that is ALREADY connected, or whose connection is ALREADY in
 * progress (e.g. our own earlier attempt, or BlueZ's own auto-reconnect),
 * is reported back as "org.bluez.Error.AlreadyConnected" / "...InProgress"
 * -- both mean the desired end state (a live A2DP transport) either already
 * holds or is already being worked on, so this file treats both as success
 * (return 0) rather than -1. Any other D-Bus error (device absent, adapter
 * not powered, genuinely refused, ...) is a real failure.
 */
#include "bt_ops.h"

#include "../../../common/m4-i18n.h"

#include <stdio.h>
#include <string.h>

#include <systemd/sd-bus.h>

static void build_device_path(const char *bt_adapter, const char *mac_address, char *out, size_t out_cap)
{
	size_t o;
	int n = snprintf(out, out_cap, "/org/bluez/%s/dev_", bt_adapter != NULL ? bt_adapter : "hci0");

	if (n < 0 || (size_t)n >= out_cap) {
		if (out_cap > 0)
			out[0] = '\0';
		return;
	}
	o = (size_t)n;
	for (const char *p = mac_address; p != NULL && *p != '\0' && o + 1 < out_cap; p++)
		out[o++] = (*p == ':') ? '_' : *p;
	out[o] = '\0';
}

static int is_benign_already_ok(const sd_bus_error *error)
{
	if (error == NULL || error->name == NULL)
		return 0;
	return strstr(error->name, "AlreadyConnected") != NULL ||
	       strstr(error->name, "InProgress") != NULL;
}

static int sdbus_connect_profile(void *ctx, const char *mac_address, const char *bt_adapter,
				  const char *uuid, int timeout_ms, char *err_buf, size_t err_buf_len)
{
	sd_bus *bus = NULL;
	sd_bus_message *call = NULL;
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	char path[192];
	int r;
	int rc = -1;

	(void)ctx;
	if (err_buf != NULL && err_buf_len > 0)
		err_buf[0] = '\0';
	if (mac_address == NULL || mac_address[0] == '\0' || uuid == NULL) {
		if (err_buf != NULL)
			snprintf(err_buf, err_buf_len, M4_T("internal error: empty mac_address/uuid",
"внутренняя ошибка: пустой mac_address/uuid"));
		return -1;
	}

	build_device_path(bt_adapter, mac_address, path, sizeof(path));

	r = sd_bus_open_system(&bus);
	if (r < 0) {
		if (err_buf != NULL)
			snprintf(err_buf, err_buf_len, "sd_bus_open_system: %s", strerror(-r));
		return -1;
	}

	r = sd_bus_message_new_method_call(bus, &call, "org.bluez", path, "org.bluez.Device1",
					    "ConnectProfile");
	if (r < 0) {
		if (err_buf != NULL)
			snprintf(err_buf, err_buf_len, "sd_bus_message_new_method_call: %s", strerror(-r));
		goto out;
	}

	r = sd_bus_message_append(call, "s", uuid);
	if (r < 0) {
		if (err_buf != NULL)
			snprintf(err_buf, err_buf_len, "sd_bus_message_append: %s", strerror(-r));
		goto out;
	}

	r = sd_bus_call(bus, call, timeout_ms > 0 ? (uint64_t)timeout_ms * 1000 : 0, &error, &reply);
	if (r < 0) {
		if (is_benign_already_ok(&error)) {
			rc = 0;
			if (err_buf != NULL)
				snprintf(err_buf, err_buf_len, M4_T("%s (%s) -- treating as success",
"%s (%s) -- считаю успехом"),
					 error.name != NULL ? error.name : "?",
					 error.message != NULL ? error.message : "");
		} else {
			rc = -1;
			if (err_buf != NULL)
				snprintf(err_buf, err_buf_len, "%s: %s",
					 error.name != NULL ? error.name : "org.bluez.Error",
					 error.message != NULL ? error.message : strerror(-r));
		}
		goto out;
	}

	rc = 0;

out:
	sd_bus_error_free(&error);
	if (reply != NULL)
		sd_bus_message_unref(reply);
	if (call != NULL)
		sd_bus_message_unref(call);
	if (bus != NULL)
		sd_bus_unref(bus);
	return rc;
}

const struct m4_bt_ops m4_bt_ops_sdbus = {
	.ctx = NULL,
	.connect_profile = sdbus_connect_profile,
};
