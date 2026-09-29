#include "loop_glue.h"

#include <errno.h>
#include <sys/epoll.h>

#include <pipewire/loop.h>

static int on_pw_loop_readable(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
	struct pw_loop *loop = userdata;
	(void)s;
	(void)fd;
	(void)revents;

	pw_loop_enter(loop);
	pw_loop_iterate(loop, 0);
	pw_loop_leave(loop);
	return 0;
}

int m4_loop_glue_attach(sd_event *event, struct pw_loop *loop, sd_event_source **out)
{
	int fd, r;

	if (event == NULL || loop == NULL || out == NULL)
		return -EINVAL;

	fd = pw_loop_get_fd(loop);
	if (fd < 0)
		return fd;

	r = sd_event_add_io(event, out, fd, EPOLLIN, on_pw_loop_readable, loop);
	if (r < 0)
		return r;

	return 0;
}
