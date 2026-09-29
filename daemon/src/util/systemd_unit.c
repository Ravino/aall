/* util/systemd_unit.c -- see systemd_unit.h. */
#include "systemd_unit.h"

#include "../../../common/m4-i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M4_SD_DESTINATION "org.freedesktop.systemd1"
#define M4_SD_PATH "/org/freedesktop/systemd1"
#define M4_SD_MANAGER_IFACE "org.freedesktop.systemd1.Manager"

static void copy_err(char *err, size_t err_len, const sd_bus_error *e, const char *fallback)
{
	if (err == NULL || err_len == 0)
		return;
	if (e != NULL && sd_bus_error_is_set(e) && e->message != NULL)
		snprintf(err, err_len, "%s", e->message);
	else
		snprintf(err, err_len, "%s", fallback != NULL ? fallback : M4_T("unknown sd-bus error",
"неизвестная ошибка sd-bus"));
}

int m4_systemd_user_connect(sd_bus **out, char *err, size_t err_len)
{
	int r;

	if (out == NULL)
		return -1;
	*out = NULL;
	r = sd_bus_default_user(out);
	if (r < 0) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len,
				 M4_T("failed to connect to the systemd --user bus (%s) -- "
				 "an active systemd --user session is needed (usually present on a "
				 "normal login; may be missing over SSH without loginctl enable-linger).",
"не удалось подключиться к пользовательской шине systemd (%s) -- "
				 "нужна активная сессия systemd --user (обычно есть при обычном входе в "
				 "систему; по SSH без loginctl enable-linger её может не быть)."),
				 strerror(-r));
		return -1;
	}
	return 0;
}

int m4_systemd_enable_unit_files(sd_bus *bus, const char *const *paths, size_t n_paths,
				  bool force, char *err, size_t err_len)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *call = NULL;
	sd_bus_message *reply = NULL;
	int r;
	size_t i;

	if (bus == NULL || paths == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, M4_T("internal error: empty arguments",
"внутренняя ошибка: пустые аргументы"));
		return -1;
	}

	r = sd_bus_message_new_method_call(bus, &call, M4_SD_DESTINATION, M4_SD_PATH,
					    M4_SD_MANAGER_IFACE, "EnableUnitFiles");
	if (r < 0)
		goto fail;
	r = sd_bus_message_open_container(call, 'a', "s");
	if (r < 0)
		goto fail;
	for (i = 0; i < n_paths; i++) {
		r = sd_bus_message_append(call, "s", paths[i]);
		if (r < 0)
			goto fail;
	}
	r = sd_bus_message_close_container(call);
	if (r < 0)
		goto fail;
	/* runtime=false (persistent, not /run/), force as requested */
	r = sd_bus_message_append(call, "bb", 0, force ? 1 : 0);
	if (r < 0)
		goto fail;
	r = sd_bus_call(bus, call, 0, &error, &reply);
	if (r < 0) {
		copy_err(err, err_len, &error, M4_T("EnableUnitFiles failed",
"EnableUnitFiles не удался"));
		sd_bus_message_unref(call);
		sd_bus_error_free(&error);
		return -1;
	}
	sd_bus_message_unref(reply);
	sd_bus_message_unref(call);
	sd_bus_error_free(&error);
	return 0;

fail:
	if (err != NULL && err_len > 0)
		snprintf(err, err_len, M4_T("failed to build the EnableUnitFiles call (%s)",
"не удалось собрать вызов EnableUnitFiles (%s)"), strerror(-r));
	if (call != NULL)
		sd_bus_message_unref(call);
	sd_bus_error_free(&error);
	return -1;
}

int m4_systemd_disable_unit_files(sd_bus *bus, const char *const *names, size_t n_names,
				   char *err, size_t err_len)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	sd_bus_message *call = NULL;
	int r;
	size_t i;

	if (bus == NULL || names == NULL) {
		if (err != NULL && err_len > 0)
			snprintf(err, err_len, M4_T("internal error: empty arguments",
"внутренняя ошибка: пустые аргументы"));
		return -1;
	}
	r = sd_bus_message_new_method_call(bus, &call, M4_SD_DESTINATION, M4_SD_PATH,
					    M4_SD_MANAGER_IFACE, "DisableUnitFiles");
	if (r < 0)
		goto fail;
	r = sd_bus_message_open_container(call, 'a', "s");
	if (r < 0)
		goto fail;
	for (i = 0; i < n_names; i++) {
		r = sd_bus_message_append(call, "s", names[i]);
		if (r < 0)
			goto fail;
	}
	r = sd_bus_message_close_container(call);
	if (r < 0)
		goto fail;
	r = sd_bus_message_append(call, "b", 0);
	if (r < 0)
		goto fail;
	r = sd_bus_call(bus, call, 0, &error, &reply);
	if (r < 0) {
		copy_err(err, err_len, &error, M4_T("DisableUnitFiles failed",
"DisableUnitFiles не удался"));
		sd_bus_error_free(&error);
		sd_bus_message_unref(call);
		return -1;
	}
	sd_bus_message_unref(reply);
	sd_bus_message_unref(call);
	sd_bus_error_free(&error);
	return 0;

fail:
	if (err != NULL && err_len > 0)
		snprintf(err, err_len, M4_T("failed to build the DisableUnitFiles call (%s)",
"не удалось собрать вызов DisableUnitFiles (%s)"), strerror(-r));
	if (call != NULL)
		sd_bus_message_unref(call);
	sd_bus_error_free(&error);
	return -1;
}

int m4_systemd_daemon_reload(sd_bus *bus, char *err, size_t err_len)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int r;

	r = sd_bus_call_method(bus, M4_SD_DESTINATION, M4_SD_PATH, M4_SD_MANAGER_IFACE, "Reload",
				&error, NULL, "");
	if (r < 0) {
		copy_err(err, err_len, &error, M4_T("Reload failed",
"Reload не удался"));
		sd_bus_error_free(&error);
		return -1;
	}
	sd_bus_error_free(&error);
	return 0;
}

static int job_call(sd_bus *bus, const char *method, const char *name, char *err, size_t err_len)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int r;

	r = sd_bus_call_method(bus, M4_SD_DESTINATION, M4_SD_PATH, M4_SD_MANAGER_IFACE, method,
				&error, &reply, "ss", name, "replace");
	if (r < 0) {
		char msg[256];

		snprintf(msg, sizeof(msg), M4_T("%s(%s) failed",
"%s(%s) не удался"), method, name);
		copy_err(err, err_len, &error, msg);
		sd_bus_error_free(&error);
		return -1;
	}
	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);
	return 0;
}

int m4_systemd_start_unit(sd_bus *bus, const char *name, char *err, size_t err_len)
{
	return job_call(bus, "StartUnit", name, err, err_len);
}

int m4_systemd_stop_unit(sd_bus *bus, const char *name, char *err, size_t err_len)
{
	return job_call(bus, "StopUnit", name, err, err_len);
}

int m4_systemd_restart_unit(sd_bus *bus, const char *name, char *err, size_t err_len)
{
	return job_call(bus, "RestartUnit", name, err, err_len);
}

int m4_systemd_get_unit_file_state(sd_bus *bus, const char *name, char *out, size_t out_len)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	const char *state = NULL;
	int r;

	if (out != NULL && out_len > 0)
		out[0] = '\0';
	r = sd_bus_call_method(bus, M4_SD_DESTINATION, M4_SD_PATH, M4_SD_MANAGER_IFACE,
				"GetUnitFileState", &error, &reply, "s", name);
	if (r < 0) {
		sd_bus_error_free(&error);
		if (out != NULL && out_len > 0)
			snprintf(out, out_len, "not-found");
		return 0; /* not-found is a valid, expected answer, not a failure */
	}
	r = sd_bus_message_read(reply, "s", &state);
	if (r >= 0 && out != NULL && out_len > 0)
		snprintf(out, out_len, "%s", state != NULL ? state : "?");
	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);
	return 0;
}

int m4_systemd_get_active_state(sd_bus *bus, const char *name, char *out, size_t out_len)
{
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	char *unit_path = NULL;
	char *state = NULL;
	int r;

	if (out != NULL && out_len > 0)
		snprintf(out, out_len, "unknown");
	r = sd_bus_call_method(bus, M4_SD_DESTINATION, M4_SD_PATH, M4_SD_MANAGER_IFACE, "GetUnit",
				&error, &reply, "s", name);
	if (r < 0) {
		sd_bus_error_free(&error);
		return 0; /* unit not loaded -- "unknown" already set, not an error for the caller */
	}
	{
		const char *p = NULL;

		r = sd_bus_message_read(reply, "o", &p);
		if (r >= 0 && p != NULL)
			unit_path = strdup(p);
		sd_bus_message_unref(reply);
		reply = NULL;
	}
	sd_bus_error_free(&error);
	if (unit_path == NULL)
		return 0;

	sd_bus_error_free(&error);
	r = sd_bus_get_property_string(bus, M4_SD_DESTINATION, unit_path,
					"org.freedesktop.systemd1.Unit", "ActiveState", &error, &state);
	if (r >= 0 && state != NULL && out != NULL && out_len > 0)
		snprintf(out, out_len, "%s", state);
	free(state);
	free(unit_path);
	sd_bus_error_free(&error);
	return 0;
}

/* Appends `dirname`/"systemd/user" to `list`/`n` (bounded by `cap`); a
 * no-op if `dirname` is NULL or empty (mirrors how systemd itself skips
 * empty path-list elements). */
static void add_unit_dir(char list[][768], size_t *n, size_t cap, const char *dirname)
{
	if (dirname == NULL || dirname[0] == '\0' || *n >= cap)
		return;
	/* "%.750s" caps the copied prefix so the compiler can prove
	 * "/systemd/user" (13 bytes) plus the NUL always fits in the
	 * 768-byte buffer, silencing -Wformat-truncation; an env var this
	 * long would already be unusable as a real directory path. */
	snprintf(list[*n], sizeof(list[0]), "%.750s/systemd/user", dirname);
	(*n)++;
}

/* Splits a ':'-separated path list (as XDG_CONFIG_DIRS/XDG_DATA_DIRS are
 * defined) and appends "<entry>/systemd/user" for each non-empty entry. */
static void add_unit_dirs_from_list(char list[][768], size_t *n, size_t cap, const char *colon_list)
{
	char buf[2048];
	char *save = NULL;
	char *tok;

	if (colon_list == NULL || colon_list[0] == '\0')
		return;
	snprintf(buf, sizeof(buf), "%s", colon_list);
	for (tok = strtok_r(buf, ":", &save); tok != NULL; tok = strtok_r(NULL, ":", &save))
		add_unit_dir(list, n, cap, tok);
}

bool m4_systemd_unit_dir_in_search_path(const char *dir)
{
	const char *home = getenv("HOME");
	const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
	const char *xdg_config_dirs = getenv("XDG_CONFIG_DIRS");
	const char *xdg_data_home = getenv("XDG_DATA_HOME");
	const char *xdg_data_dirs = getenv("XDG_DATA_DIRS");
	char list[16][768];
	char fallback[768];
	size_t n = 0;
	size_t i;

	if (dir == NULL || dir[0] == '\0')
		return false;

	/* $XDG_CONFIG_HOME/systemd/user, or ~/.config/systemd/user */
	if (xdg_config_home != NULL && xdg_config_home[0] != '\0') {
		add_unit_dir(list, &n, 16, xdg_config_home);
	} else if (home != NULL && home[0] != '\0') {
		snprintf(fallback, sizeof(fallback), "%s/.config", home);
		add_unit_dir(list, &n, 16, fallback);
	}
	/* $XDG_CONFIG_DIRS (default /etc/xdg), plus the hardcoded /etc/systemd/user */
	if (xdg_config_dirs != NULL && xdg_config_dirs[0] != '\0')
		add_unit_dirs_from_list(list, &n, 16, xdg_config_dirs);
	else
		add_unit_dir(list, &n, 16, "/etc/xdg");
	add_unit_dir(list, &n, 16, "/etc");

	/* $XDG_DATA_HOME/systemd/user, or ~/.local/share/systemd/user */
	if (xdg_data_home != NULL && xdg_data_home[0] != '\0') {
		add_unit_dir(list, &n, 16, xdg_data_home);
	} else if (home != NULL && home[0] != '\0') {
		snprintf(fallback, sizeof(fallback), "%s/.local/share", home);
		add_unit_dir(list, &n, 16, fallback);
	}
	/* $XDG_DATA_DIRS (default /usr/local/share:/usr/share) */
	if (xdg_data_dirs != NULL && xdg_data_dirs[0] != '\0')
		add_unit_dirs_from_list(list, &n, 16, xdg_data_dirs);
	else
		add_unit_dirs_from_list(list, &n, 16, "/usr/local/share:/usr/share");

	/* The two fixed lib/ entries -- systemd itself never derives these
	 * from any XDG variable. */
	add_unit_dir(list, &n, 16, "/usr/local/lib");
	add_unit_dir(list, &n, 16, "/usr/lib");

	for (i = 0; i < n; i++)
		if (strcmp(list[i], dir) == 0)
			return true;
	return false;
}
