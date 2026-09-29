/* pw/graph_wait.c -- see graph_wait.h. */
#include "graph_wait.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include <systemd/sd-daemon.h>

uint64_t m4_graph_wait_now_usec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

int m4_graph_wait_step(const struct graph_ops *ops, void *handle, sd_event *event,
		       uint64_t timeout_usec)
{
	int state = event != NULL ? sd_event_get_state(event) : -EINVAL;

	if (state == SD_EVENT_INITIAL)
		return sd_event_run(event, timeout_usec);

	if (ops != NULL && ops->iterate != NULL && handle != NULL) {
		uint64_t ms = timeout_usec / 1000;

		if (ms < 1)
			ms = 1;
		if (ms > M4_GRAPH_WAIT_SLICE_MS)
			ms = M4_GRAPH_WAIT_SLICE_MS;
		if (getenv("WATCHDOG_USEC") != NULL)
			sd_notify(0, "WATCHDOG=1");
		return ops->iterate(handle, (int)ms);
	}
	return -EBUSY;
}
