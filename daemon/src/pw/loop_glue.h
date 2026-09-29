/* pw/loop_glue.h -- integrates a `struct pw_loop` into an `sd_event` loop
 * (sd_event_add_io on pw_loop_get_fd + pw_loop_iterate(l,0)).
 * This is the same pattern WirePlumber itself uses to embed pw_loop into a
 * GMainLoop (a GSource on pw_loop_get_fd() whose dispatch calls
 * pw_loop_enter()/pw_loop_iterate()/pw_loop_leave()) -- pw_loop is
 * internally one epoll fd multiplexing everything added to it (timers,
 * idles, the core's own socket once connected), so a single sd_event IO
 * source, re-armed by sd-event automatically since it isn't ONESHOT, is
 * enough; no separate integration is needed per pw_loop source.
 */
#ifndef MOMENTUM4_PW_LOOP_GLUE_H
#define MOMENTUM4_PW_LOOP_GLUE_H

#include <systemd/sd-event.h>

struct pw_loop;

#ifdef __cplusplus
extern "C" {
#endif

/* Adds an sd_event IO source that pumps `loop` on every readiness of its fd.
 * `loop` must outlive the returned source (caller keeps owning it, as with
 * every other pw_loop user in this codebase). Returns 0 and sets *out on
 * success, a negative errno on failure (pw_loop_get_fd() failing, or
 * sd_event_add_io() failing) -- *out is left untouched on failure. */
int m4_loop_glue_attach(sd_event *event, struct pw_loop *loop, sd_event_source **out);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_PW_LOOP_GLUE_H */
