/*
 * Running a GMainContext the way an application with a loop of its own does,
 * step by step (prepare, query, poll, check, dispatch), so that the tests can
 * tell when nothing is left to wait for: a GMainLoop only stops when asked to.
 */
#ifndef GLIB_ITERATE_H
#define GLIB_ITERATE_H

#include <glib.h>
#include <stdlib.h>

/* How many descriptors an empty context polls: GLib always includes the one
 * that wakes the context up. */
static int
glib_baseline_fds(void)
{
	static int n = -1;

	if (n < 0) {
		GMainContext *c = g_main_context_new();
		gint prio, timeout;

		g_main_context_acquire(c);
		g_main_context_prepare(c, &prio);
		n = g_main_context_query(c, prio, &timeout, NULL, 0);
		g_main_context_check(c, prio, NULL, 0);
		g_main_context_release(c);
		g_main_context_unref(c);
	}
	return n;
}

/* One iteration. Returns 0 if there was nothing to wait for (no descriptor of
 * ours, no timer, nothing ready), in which case nothing was dispatched. */
static int
glib_iterate(GMainContext *ctx)
{
	GPollFD *fds = NULL;
	gint prio, timeout, n = 0, cap = 0, ready;
	int rv = 1;

	if (!g_main_context_acquire(ctx))
		abort();
	ready = g_main_context_prepare(ctx, &prio);
	for (;;) {
		n = g_main_context_query(ctx, prio, &timeout, fds, cap);
		if (n <= cap)
			break;
		cap = n;
		fds = realloc(fds, (size_t) cap * sizeof(*fds));
	}
	if (!ready && timeout == -1 && n <= glib_baseline_fds()) {
		rv = 0;
	} else {
		g_poll(fds, (guint) n, timeout);
	}
	if (g_main_context_check(ctx, prio, fds, n))
		g_main_context_dispatch(ctx);
	g_main_context_release(ctx);
	free(fds);
	return rv;
}

#endif
