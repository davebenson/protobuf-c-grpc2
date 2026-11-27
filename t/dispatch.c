/*
 * The ProtobufCRPCDispatch adapter itself: timers, hooks, the connect
 * contract (completion is never reported from inside the call; cancelling
 * reports nothing), sharing a dispatch with other users of it, and
 * reference counting.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2-dispatch.h"

static void *
test_alloc(void *data, size_t size)
{
	(void) data;
	return malloc(size);
}

static void
test_free(void *data, void *ptr)
{
	(void) data;
	free(ptr);
}

static ProtobufCAllocator test_allocator = { test_alloc, test_free, NULL };

static ProtobufCRPCDispatch *d;
static ProtobufCGrpc2IoSystem *io;

/* Something to wait for: a watched descriptor, a timer or an idle function. */
static int
alive(void)
{
	return d->n_notifies_desired != 0 || d->has_timeout || d->has_idle;
}

static void
run_until_idle(void)
{
	while (alive())
		protobuf_c_rpc_dispatch_run(d);
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
	puts("dispatch: timers ok");
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
	protobuf_c_rpc_dispatch_run(d);
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
	puts("dispatch: hooks ok");
}

/* The dispatch is shared: this program's own descriptors and timers keep
 * working next to the I/O system's. */
static int pipe_fds[2];
static int pipe_events, direct_timer_fired;

static void
on_pipe(int fd, unsigned events, void *data)
{
	char c;
	(void) data;
	assert(fd == pipe_fds[0] && (events & PROTOBUF_C_RPC_EVENT_READABLE));
	assert(read(fd, &c, 1) == 1);
	pipe_events++;
}

static void
direct_timer(ProtobufCRPCDispatch *disp, void *data)
{
	(void) disp;
	(void) data;
	direct_timer_fired++;
}

static void
test_shared(void)
{
	void *t = io->timer_new(io, on_timer, (void *) 7L);

	assert(pipe(pipe_fds) == 0);
	n_fired = 0;
	protobuf_c_rpc_dispatch_watch_fd(d, pipe_fds[0],
					 PROTOBUF_C_RPC_EVENT_READABLE,
					 on_pipe, NULL);
	protobuf_c_rpc_dispatch_add_timer_millis(d, 20, direct_timer, NULL);
	io->timer_start(io, t, 40);
	assert(write(pipe_fds[1], "x", 1) == 1);
	while (n_fired == 0)
		protobuf_c_rpc_dispatch_run(d);
	assert(pipe_events == 1 && direct_timer_fired == 1 && n_fired == 1);
	protobuf_c_rpc_dispatch_close_fd(d, pipe_fds[0]);
	close(pipe_fds[1]);
	run_until_idle();
	io->timer_free(io, t);
	puts("dispatch: shared ok");
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
	puts("dispatch: connect ok");
}

int
main(void)
{
	d = protobuf_c_rpc_dispatch_new(&test_allocator);
	assert(d != NULL);
	io = protobuf_c_grpc2_io_system_new_dispatch(d);
	assert(io != NULL);
	io->ref(io);	/* claims the floating creation reference */

	test_timers();
	test_hooks();
	test_shared();
	test_connect();
	io->destroy(io);

	/* the first ref claims the creation ref; the second adds one (run this
	 * under a leak checker: the system is freed by the last release) */
	io = protobuf_c_grpc2_io_system_new_dispatch(d);
	io->ref(io);
	io->ref(io);
	io->destroy(io);
	io->destroy(io);

	/* a system nobody took is freed by one release */
	io = protobuf_c_grpc2_io_system_new_dispatch(d);
	io->destroy(io);

	/* NULL: the default dispatch */
	io = protobuf_c_grpc2_io_system_new_dispatch(NULL);
	assert(io != NULL);
	io->destroy(io);
	protobuf_c_rpc_dispatch_destroy_default();

	protobuf_c_rpc_dispatch_free(d);
	puts("dispatch: ok");
	return 0;
}
