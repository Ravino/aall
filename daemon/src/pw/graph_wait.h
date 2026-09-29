/* pw/graph_wait.h -- ONE step of a bounded "wait until the graph confirms X"
 * poll loop (pw/callfsm_graph_adapter.c set_profile/mic-volume confirm,
 * pw/repair_a2dp.c enum-wait), usable both from the top level of the
 * program AND from inside an sd-event callback.
 *
 * Why this exists: sd_event_run() does
 * not nest. Called while the same sd_event is already dispatching (state
 * SD_EVENT_RUNNING -- i.e. from ANY callback: ctl.sock request, tick
 * timer, micdetect, the MPRIS player's D-Bus handler) it returns -EBUSY at
 * once. Every live call path of momentum4d is such a callback, so the old
 * "nested sd_event_run()" poll loops gave up on their first step: the
 * profile switch request was queued, the loop reported "not confirmed",
 * and only after the callback returned did PipeWire actually get the
 * message (journal: CALL_ON failure logged BEFORE any
 * audio change). The fake-graph tests never saw it -- the fake confirms
 * synchronously, and they call from the top level.
 *
 * Behaviour:
 *   - event not dispatching (SD_EVENT_INITIAL): sd_event_run(event,
 *     timeout) -- exactly the old behaviour (top-level CLI `repair-a2dp`,
 *     tests, scenario_runner's timer-driven fakes keep working unchanged);
 *   - event dispatching AND ops->iterate != NULL: pump only the graph's own
 *     PipeWire loop for <= M4_GRAPH_WAIT_SLICE_MS, and ping the systemd
 *     watchdog (sd_notify WATCHDOG=1; a no-op outside systemd) so a long
 *     confirm (mSBC 10 s + CVSD 10 s + mic 10 s) can't trip WatchdogSec=30;
 *   - otherwise: -EBUSY, as before (caller treats it as "gave up").
 * Time must be measured with m4_graph_wait_now_usec() (CLOCK_MONOTONIC via
 * clock_gettime) -- NOT sd_event_now(), which is frozen at the current
 * iteration's timestamp while dispatching and would never reach a deadline.
 */
#ifndef MOMENTUM4_PW_GRAPH_WAIT_H
#define MOMENTUM4_PW_GRAPH_WAIT_H

#include <stdint.h>

#include <systemd/sd-event.h>

#include "../core/graph_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M4_GRAPH_WAIT_SLICE_MS 50

uint64_t m4_graph_wait_now_usec(void);

/* >= 0: something may have happened, re-check your predicate.
 * < 0: cannot wait at all (caller gives up). */
int m4_graph_wait_step(const struct graph_ops *ops, void *handle, sd_event *event,
		       uint64_t timeout_usec);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_PW_GRAPH_WAIT_H */
