/* tests/systemd_stub.c -- minimal in-process fake of
 * org.freedesktop.systemd1.Manager, used for dry-run install tests
 * in an isolated $HOME with a stub user-manager.
 *
 * Owns "org.freedesktop.systemd1" on WHATEVER bus $DBUS_SESSION_BUS_ADDRESS
 * points at -- run this ONLY under `dbus-run-session` (see
 * test_lifecycle_cli.sh), never against a real session bus (see
 * util/systemd_unit.h's own header comment for exactly why that matters --
 * this project found out the hard way that $HOME/$XDG_RUNTIME_DIR
 * overrides alone do NOT isolate sd_bus_default_user()).
 *
 * Implements just enough of Manager to exercise cli/lifecycle.c's call
 * sequence end to end: EnableUnitFiles/DisableUnitFiles/Reload/StartUnit/
 * StopUnit/RestartUnit/GetUnitFileState/GetUnit, plus ONE fake unit object
 * (always the same object path, regardless of which unit name GetUnit was
 * asked about -- this stub does not need to distinguish between
 * momentum4d.service/momentum4-helper.socket/wireplumber.service for the
 * CLI-integration tests that use it) exposing org.freedesktop.systemd1.Unit's
 * ActiveState property. Every call is logged to stdout (one line each) so
 * the driving shell script can grep for "the right calls happened in the
 * right order" without needing a real systemd anywhere in the loop.
 *
 * Exits after `--calls N` method calls have been served (default: run
 * until stdin closes/SIGTERM) -- the test script pipes a sentinel through
 * stdin and waits for this process, avoiding a fixed sleep.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-bus.h>

static volatile sig_atomic_t g_stop;

static void on_term(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* sd_bus_message_read_strv() hands back a malloc()'d, NUL-terminated
 * char** whose elements are ALSO malloc()'d (sd_bus_message_read_strv(3)) --
 * public libsystemd has no exported "strv_free" helper for it, so this
 * stub frees it by hand. */
static void local_strv_free(char **strv)
{
	char **p;

	if (strv == NULL)
		return;
	for (p = strv; *p != NULL; p++)
		free(*p);
	free(strv);
}

static int method_log_ok(sd_bus_message *m, const char *name)
{
	printf("STUB: %s\n", name);
	fflush(stdout);
	return sd_bus_reply_method_return(m, "");
}

static int on_enable_unit_files(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	char **files = NULL;
	int r;
	int runtime, force;

	(void)userdata;
	(void)ret_error;
	r = sd_bus_message_read_strv(m, &files);
	if (r < 0)
		return r;
	r = sd_bus_message_read(m, "bb", &runtime, &force);
	if (r < 0) {
		local_strv_free(files);
		return r;
	}
	if (files != NULL) {
		char **p;

		for (p = files; *p != NULL; p++) {
			printf("STUB: EnableUnitFiles(%s)\n", *p);
			fflush(stdout);
		}
	}
	local_strv_free(files);
	{
		int rc = sd_bus_reply_method_return(m, "ba(sss)", 0, 0);

		return rc;
	}
}

static int on_disable_unit_files(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	char **names = NULL;
	int r;
	int runtime;

	(void)userdata;
	(void)ret_error;
	r = sd_bus_message_read_strv(m, &names);
	if (r < 0)
		return r;
	r = sd_bus_message_read(m, "b", &runtime);
	if (r < 0) {
		local_strv_free(names);
		return r;
	}
	if (names != NULL) {
		char **p;

		for (p = names; *p != NULL; p++) {
			printf("STUB: DisableUnitFiles(%s)\n", *p);
			fflush(stdout);
		}
	}
	local_strv_free(names);
	return sd_bus_reply_method_return(m, "a(sss)", 0);
}

static int on_reload(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	(void)userdata;
	(void)ret_error;
	return method_log_ok(m, "Reload");
}

static int on_job_method(sd_bus_message *m, void *userdata, sd_bus_error *ret_error, const char *tag)
{
	const char *name = NULL, *mode = NULL;
	int r;

	(void)userdata;
	(void)ret_error;
	r = sd_bus_message_read(m, "ss", &name, &mode);
	if (r < 0)
		return r;
	printf("STUB: %s(%s, %s)\n", tag, name, mode);
	fflush(stdout);
	return sd_bus_reply_method_return(m, "o", "/org/freedesktop/systemd1/job/1");
}

static int on_start_unit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	return on_job_method(m, userdata, ret_error, "StartUnit");
}

static int on_stop_unit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	return on_job_method(m, userdata, ret_error, "StopUnit");
}

static int on_restart_unit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	return on_job_method(m, userdata, ret_error, "RestartUnit");
}

static int on_get_unit_file_state(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	const char *name = NULL;
	int r;

	(void)userdata;
	(void)ret_error;
	r = sd_bus_message_read(m, "s", &name);
	if (r < 0)
		return r;
	printf("STUB: GetUnitFileState(%s)\n", name);
	fflush(stdout);
	return sd_bus_reply_method_return(m, "s", "enabled");
}

#define FAKE_UNIT_PATH "/org/freedesktop/systemd1/unit/fake_2eservice"

static int on_get_unit(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	const char *name = NULL;
	int r;

	(void)userdata;
	(void)ret_error;
	r = sd_bus_message_read(m, "s", &name);
	if (r < 0)
		return r;
	printf("STUB: GetUnit(%s)\n", name);
	fflush(stdout);
	return sd_bus_reply_method_return(m, "o", FAKE_UNIT_PATH);
}

static const sd_bus_vtable manager_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("EnableUnitFiles", "asbb", "ba(sss)", on_enable_unit_files, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("DisableUnitFiles", "asb", "a(sss)", on_disable_unit_files, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Reload", "", "", on_reload, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("StartUnit", "ss", "o", on_start_unit, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("StopUnit", "ss", "o", on_stop_unit, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RestartUnit", "ss", "o", on_restart_unit, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetUnitFileState", "s", "s", on_get_unit_file_state, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetUnit", "s", "o", on_get_unit, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

static const char *active_state_get(void)
{
	return "active";
}

static int prop_active_state(sd_bus *bus, const char *path, const char *interface, const char *property,
			      sd_bus_message *reply, void *userdata, sd_bus_error *ret_error)
{
	(void)bus;
	(void)path;
	(void)interface;
	(void)property;
	(void)userdata;
	(void)ret_error;
	return sd_bus_message_append(reply, "s", active_state_get());
}

static const sd_bus_vtable unit_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("ActiveState", "s", prop_active_state, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

int main(int argc, char **argv)
{
	sd_bus *bus = NULL;
	sd_bus_slot *manager_slot = NULL;
	sd_bus_slot *unit_slot = NULL;
	int r;

	(void)argc;
	(void)argv;
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	r = sd_bus_default_user(&bus);
	if (r < 0) {
		fprintf(stderr, "systemd_stub: sd_bus_default_user: %s\n", strerror(-r));
		return 1;
	}
	r = sd_bus_add_object_vtable(bus, &manager_slot, "/org/freedesktop/systemd1",
				      "org.freedesktop.systemd1.Manager", manager_vtable, NULL);
	if (r < 0) {
		fprintf(stderr, "systemd_stub: add_object_vtable(Manager): %s\n", strerror(-r));
		return 1;
	}
	r = sd_bus_add_object_vtable(bus, &unit_slot, FAKE_UNIT_PATH, "org.freedesktop.systemd1.Unit",
				      unit_vtable, NULL);
	if (r < 0) {
		fprintf(stderr, "systemd_stub: add_object_vtable(Unit): %s\n", strerror(-r));
		return 1;
	}
	r = sd_bus_request_name(bus, "org.freedesktop.systemd1", 0);
	if (r < 0) {
		fprintf(stderr, "systemd_stub: request_name: %s\n", strerror(-r));
		return 1;
	}

	printf("STUB: ready\n");
	fflush(stdout);

	while (!g_stop) {
		r = sd_bus_process(bus, NULL);
		if (r < 0) {
			fprintf(stderr, "systemd_stub: process: %s\n", strerror(-r));
			break;
		}
		if (r > 0)
			continue;
		r = sd_bus_wait(bus, 200000); /* 200ms, then re-check g_stop */
		if (r < 0 && r != -EINTR) {
			fprintf(stderr, "systemd_stub: wait: %s\n", strerror(-r));
			break;
		}
	}

	sd_bus_slot_unref(unit_slot);
	sd_bus_slot_unref(manager_slot);
	sd_bus_unref(bus);
	return 0;
}
