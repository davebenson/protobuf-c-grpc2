/*
 * The GLib adapter itself: timers, hooks, the connect contract (completion is
 * never reported from inside the call; cancelling reports nothing), sharing a
 * context with other users of it, and reference counting.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>

#include "free-port.h"

#include "protobuf-c-grpc2/protobuf-c-grpc2-glib.h"
#include "glib-iterate.h"

static GMainContext *ctx;
static ProtobufCGrpc2IoSystem *io;

/* Something to wait for: a descriptor, a timer or an idle task. */
static int
alive(void)
{
	GPollFD fd;
	gint prio, timeout, ready;
	int rv;

	g_main_context_acquire(ctx);
	ready = g_main_context_prepare(ctx, &prio);
	rv = g_main_context_query(ctx, prio, &timeout, &fd, 0) >
	     glib_baseline_fds() || ready || timeout != -1;
	g_main_context_check(ctx, prio, &fd, 0);
	g_main_context_release(ctx);
	return rv;
}

static void
run_until_idle(void)
{
	while (glib_iterate(ctx))
		;
}

static int fired[8], n_fired;
static void *timers[8];

static void
on_timer(void *arg)
{
	fired[n_fired++] = (int) (long) arg;
}

static void
free_other(void *arg)			/* frees timer 2 from timer 1's callback */
{
	on_timer(arg);
	io->timer_free(io, timers[2]);
	timers[2] = NULL;
}

static int rearmed;
static void
rearm(void *arg)			/* restarts itself with 0 ms: must not starve */
{
	(void) arg;
	if (++rearmed < 3)
		io->timer_start(io, timers[3], 0);
}

static void
test_timers(void)
{
	n_fired = 0;
	timers[0] = io->timer_new(io, on_timer, (void *) 10L);
	timers[1] = io->timer_new(io, free_other, (void *) 11L);
	timers[2] = io->timer_new(io, on_timer, (void *) 12L);
	timers[3] = io->timer_new(io, rearm, NULL);
	io->timer_start(io, timers[0], 60);
	io->timer_start(io, timers[1], 20);
	io->timer_start(io, timers[2], 40);	/* freed before it is due */
	io->timer_start(io, timers[3], 0);
	run_until_idle();
	assert(n_fired == 2 && fired[0] == 11 && fired[1] == 10);
	assert(rearmed == 3);

	/* stop cancels; restart works */
	n_fired = 0;
	io->timer_start(io, timers[0], 10000);
	io->timer_stop(io, timers[0]);
	run_until_idle();			/* returns at once: idle */
	assert(n_fired == 0);
	io->timer_start(io, timers[0], 1);
	run_until_idle();
	assert(n_fired == 1);

	/* restarting a pending timer replaces its deadline */
	n_fired = 0;
	io->timer_start(io, timers[0], 10000);
	io->timer_start(io, timers[0], 1);
	run_until_idle();
	assert(n_fired == 1);

	io->timer_free(io, timers[0]);
	io->timer_free(io, timers[1]);
	io->timer_free(io, timers[3]);
	puts("glib: timers ok");
}

static int hook_calls;
static void
hook(void *arg)
{
	(void) arg;
	hook_calls++;
}

static void *self_hook;
static int self_hook_calls;
static void
self_removing_hook(void *arg)
{
	(void) arg;
	self_hook_calls++;
	io->remove_iteration_hook(io, self_hook);
}

static void
test_hooks(void)
{
	void *h = io->add_iteration_hook(io, hook, NULL);
	void *t = io->timer_new(io, on_timer, (void *) 1L);
	int before;

	assert(h != NULL && t != NULL);
	n_fired = 0;
	io->timer_start(io, t, 30);
	before = hook_calls;
	/* nothing is due: one iteration runs the hook before it waits */
	glib_iterate(ctx);
	assert(hook_calls > before);
	run_until_idle();
	assert(n_fired == 1);
	/* hooks alone do not keep the loop alive */
	assert(!alive());
	io->remove_iteration_hook(io, h);

	/* a hook may remove itself */
	self_hook_calls = 0;
	self_hook = io->add_iteration_hook(io, self_removing_hook, NULL);
	io->timer_start(io, t, 1);
	run_until_idle();
	io->timer_start(io, t, 1);
	run_until_idle();
	assert(self_hook_calls == 1);

	/* a removed hook is not called */
	before = hook_calls;
	io->timer_start(io, t, 1);
	run_until_idle();
	assert(hook_calls == before);
	io->timer_free(io, t);
	puts("glib: hooks ok");
}

/* The context is shared: this program's own descriptors and timers keep
 * working next to the I/O system's. */
static int pipe_fds[2];
static int pipe_events, direct_timer_fired;

static gboolean
on_pipe(gint fd, GIOCondition cond, gpointer data)
{
	char c;
	(void) data;
	assert(fd == pipe_fds[0] && (cond & G_IO_IN));
	assert(read(fd, &c, 1) == 1);
	pipe_events++;
	return G_SOURCE_REMOVE;
}

static gboolean
direct_timer(gpointer data)
{
	(void) data;
	direct_timer_fired++;
	return G_SOURCE_REMOVE;
}

static void
test_shared(void)
{
	void *t = io->timer_new(io, on_timer, (void *) 7L);
	GSource *ps, *ts;

	assert(pipe(pipe_fds) == 0);
	n_fired = 0;
	ps = g_unix_fd_source_new(pipe_fds[0], G_IO_IN);
	g_source_set_callback(ps, (GSourceFunc) on_pipe, NULL, NULL);
	g_source_attach(ps, ctx);
	g_source_unref(ps);
	ts = g_timeout_source_new(20);
	g_source_set_callback(ts, direct_timer, NULL, NULL);
	g_source_attach(ts, ctx);
	g_source_unref(ts);
	io->timer_start(io, t, 40);
	assert(write(pipe_fds[1], "x", 1) == 1);
	while (n_fired == 0)
		glib_iterate(ctx);
	assert(pipe_events == 1 && direct_timer_fired == 1 && n_fired == 1);
	close(pipe_fds[0]);
	close(pipe_fds[1]);
	run_until_idle();
	io->timer_free(io, t);
	puts("glib: shared ok");
}

static GMainLoop *main_loop;
static void
quit_loop(void *arg)
{
	(void) arg;
	g_main_loop_quit(main_loop);
}

/* what an application does: g_main_loop_run() on the context */
static void
test_main_loop(void)
{
	void *t = io->timer_new(io, quit_loop, NULL);
	void *h = io->add_iteration_hook(io, hook, NULL);
	int before = hook_calls;

	main_loop = g_main_loop_new(ctx, FALSE);
	io->timer_start(io, t, 20);
	g_main_loop_run(main_loop);
	assert(hook_calls > before);
	g_main_loop_unref(main_loop);
	io->remove_iteration_hook(io, h);
	io->timer_free(io, t);
	puts("glib: main loop ok");
}

static void *accepted_stream;
static void
on_accepted(void *data, void *stream)
{
	(void) data;
	accepted_stream = stream;
}

static int connect_calls;
static int connect_error;
static void *connect_stream;
static int in_call;

static void
on_connected(void *data, void *stream, int error)
{
	(void) data;
	assert(!in_call);	/* never from inside the connect() call */
	connect_calls++;
	connect_error = error;
	connect_stream = stream;
}

static void
test_connect(void)
{
	void *handle;

	/* nothing listens on port 1: failure, reported from the loop */
	connect_calls = 0;
	in_call = 1;
	assert(io->connect(io, "127.0.0.1", 1, on_connected, NULL, &handle) == 0);
	in_call = 0;
	assert(connect_calls == 0);
	run_until_idle();
	assert(connect_calls == 1 && connect_error != 0 && connect_stream == NULL);

	/* a name that does not resolve fails the same way */
	connect_calls = 0;
	in_call = 1;
	assert(io->connect(io, "no-such-host.invalid", 80, on_connected, NULL,
			   &handle) == 0);
	in_call = 0;
	run_until_idle();
	assert(connect_calls == 1 && connect_error != 0);

	/* cancelling reports nothing, whether or not the attempt had failed */
	connect_calls = 0;
	assert(io->connect(io, "127.0.0.1", 1, on_connected, NULL, &handle) == 0);
	io->cancel_connect(io, handle);
	run_until_idle();
	assert(connect_calls == 0);
	assert(io->connect(io, "no-such-host.invalid", 80, on_connected, NULL,
			   &handle) == 0);
	io->cancel_connect(io, handle);
	run_until_idle();
	assert(connect_calls == 0);

	/* cancelling while the lookup is under way (a name that resolves) */
	assert(io->connect(io, "localhost", 1, on_connected, NULL, &handle) == 0);
	io->cancel_connect(io, handle);
	run_until_idle();
	assert(connect_calls == 0);

	/* a name that resolves, and a listener to connect to */
	{
		void *listener, *stream;
		unsigned port = test_free_port();

		assert(io->listen(io, "127.0.0.1", port, on_accepted, NULL,
				  &listener) == 0);
		connect_calls = 0;
		connect_stream = NULL;
		in_call = 1;
		assert(io->connect(io, "localhost", port, on_connected, NULL,
				   &handle) == 0);
		in_call = 0;
		while (connect_calls == 0 || accepted_stream == NULL)
			glib_iterate(ctx);
		assert(connect_calls == 1 && connect_error == 0);
		stream = connect_stream;
		assert(stream != NULL);
		io->stream_close(io, stream);		/* never started: no callback */
		io->stream_close(io, accepted_stream);
		io->unlisten(io, listener);
		run_until_idle();
	}
	puts("glib: connect ok");
}

int
main(void)
{
	ctx = g_main_context_new();
	io = protobuf_c_grpc2_io_system_new_glib(ctx);
	assert(io != NULL);
	io->ref(io);	/* claims the floating creation reference */

	test_timers();
	test_hooks();
	test_shared();
	test_main_loop();
	test_connect();
	io->destroy(io);

	/* the first ref claims the creation ref; the second adds one (run this
	 * under a leak checker: the system is freed by the last release) */
	io = protobuf_c_grpc2_io_system_new_glib(ctx);
	io->ref(io);
	io->ref(io);
	io->destroy(io);
	io->destroy(io);

	/* a system nobody took is freed by one release */
	io = protobuf_c_grpc2_io_system_new_glib(ctx);
	io->destroy(io);

	/* NULL: the default context */
	io = protobuf_c_grpc2_io_system_new_glib(NULL);
	assert(io != NULL);
	io->destroy(io);

	g_main_context_unref(ctx);
	puts("glib: ok");
	return 0;
}
