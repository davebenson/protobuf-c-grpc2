/*
 * The poll(2) event loop itself: timers, hooks, run_once/stop/close, and the
 * connect contract (completion is never reported from inside the call;
 * cancelling reports nothing).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2-poll.h"

static ProtobufCGrpc2IoSystem *
new_io(ProtobufCGrpc2PollLoop *loop)
{
	ProtobufCGrpc2PollOptions o = PROTOBUF_C_GRPC2_POLL_OPTIONS_INIT;

	o.loop = loop;
	return protobuf_c_grpc2_io_system_new_poll(&o);
}

static ProtobufCGrpc2PollLoop *loop;
static ProtobufCGrpc2IoSystem *io;

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
	protobuf_c_grpc2_poll_loop_run(loop);
	assert(n_fired == 2 && fired[0] == 11 && fired[1] == 10);
	assert(rearmed == 3);

	/* stop cancels; restart works */
	n_fired = 0;
	io->timer_start(io, timers[0], 10000);
	io->timer_stop(io, timers[0]);
	protobuf_c_grpc2_poll_loop_run(loop);	/* returns at once: idle */
	assert(n_fired == 0);
	io->timer_start(io, timers[0], 1);
	protobuf_c_grpc2_poll_loop_run(loop);
	assert(n_fired == 1);

	io->timer_free(io, timers[0]);
	io->timer_free(io, timers[1]);
	io->timer_free(io, timers[3]);
	puts("poll: timers ok");
}

static int hook_calls;
static void
hook(void *arg)
{
	(void) arg;
	hook_calls++;
}

static void
test_hooks_and_run_once(void)
{
	void *h = io->add_iteration_hook(io, hook, NULL);
	void *t = io->timer_new(io, on_timer, (void *) 1L);
	int before;

	assert(h != NULL && t != NULL);
	n_fired = 0;
	io->timer_start(io, t, 30);
	before = hook_calls;
	/* nothing is due: run_once waits at most the given time */
	assert(protobuf_c_grpc2_poll_loop_run_once(loop, 0) == 1);
	assert(n_fired == 0 && hook_calls > before);
	/* the hook runs after the timer callback of the same iteration, before
	 * the loop would block again */
	while (protobuf_c_grpc2_poll_loop_run_once(loop, 100))
		;
	assert(n_fired == 1);
	/* hooks alone do not keep the loop alive */
	protobuf_c_grpc2_poll_loop_run(loop);
	io->remove_iteration_hook(io, h);
	io->timer_free(io, t);
	puts("poll: hooks and run_once ok");
}

static void
stopper(void *arg)
{
	(void) arg;
	protobuf_c_grpc2_poll_loop_stop(loop);
}

static void
test_stop_and_close(void)
{
	void *t = io->timer_new(io, stopper, NULL);
	void *t2 = io->timer_new(io, on_timer, (void *) 2L);

	n_fired = 0;
	io->timer_start(io, t, 10);
	io->timer_start(io, t2, 200);
	protobuf_c_grpc2_poll_loop_run(loop);	/* returns when stopped */
	assert(n_fired == 0);
	/* still alive: the loop cannot be closed */
	assert(protobuf_c_grpc2_poll_loop_close(loop) == -1);
	io->timer_free(io, t);
	io->timer_free(io, t2);
	puts("poll: stop and close ok");
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
	ProtobufCGrpc2PollLoop *l2;

	/* nothing listens on port 1: failure, reported from the loop */
	connect_calls = 0;
	in_call = 1;
	assert(io->connect(io, "127.0.0.1", 1, on_connected, NULL, &handle) == 0);
	in_call = 0;
	assert(connect_calls == 0);
	protobuf_c_grpc2_poll_loop_run(loop);
	assert(connect_calls == 1 && connect_error != 0 && connect_stream == NULL);

	/* a name that does not resolve fails the same way */
	connect_calls = 0;
	in_call = 1;
	assert(io->connect(io, "no-such-host.invalid", 80, on_connected, NULL,
			   &handle) == 0);
	in_call = 0;
	protobuf_c_grpc2_poll_loop_run(loop);
	assert(connect_calls == 1 && connect_error != 0);

	/* cancelling reports nothing, whether or not the attempt had failed */
	connect_calls = 0;
	assert(io->connect(io, "127.0.0.1", 1, on_connected, NULL, &handle) == 0);
	io->cancel_connect(io, handle);
	protobuf_c_grpc2_poll_loop_run(loop);
	assert(connect_calls == 0);
	assert(io->connect(io, "no-such-host.invalid", 80, on_connected, NULL,
			   &handle) == 0);
	io->cancel_connect(io, handle);
	protobuf_c_grpc2_poll_loop_run(loop);
	assert(connect_calls == 0);

	/* a second loop is independent */
	l2 = protobuf_c_grpc2_poll_loop_new();
	assert(l2 != NULL && protobuf_c_grpc2_poll_loop_close(l2) == 0);
	puts("poll: connect ok");
}

int
main(void)
{
	/* options are required, and so is a loop in them */
	{
		ProtobufCGrpc2PollOptions none = PROTOBUF_C_GRPC2_POLL_OPTIONS_INIT;
		ProtobufCGrpc2PollOptions bad = { 0 };

		assert(protobuf_c_grpc2_io_system_new_poll(NULL) == NULL);
		assert(protobuf_c_grpc2_io_system_new_poll(&none) == NULL);
		assert(protobuf_c_grpc2_io_system_new_poll(&bad) == NULL);
	}

	loop = protobuf_c_grpc2_poll_loop_new();
	assert(loop != NULL);
	io = new_io(loop);
	assert(io != NULL);
	io->ref(io);	/* claims the floating creation reference */

	test_timers();
	test_hooks_and_run_once();
	test_stop_and_close();
	test_connect();

	/* reference counting: our ref is the last one */
	assert(protobuf_c_grpc2_poll_loop_close(loop) == -1);
	io->destroy(io);
	assert(protobuf_c_grpc2_poll_loop_close(loop) == 0);

	/* the first ref claims the creation ref; the second adds one */
	loop = protobuf_c_grpc2_poll_loop_new();
	io = new_io(loop);
	io->ref(io);
	io->ref(io);
	io->destroy(io);
	assert(protobuf_c_grpc2_poll_loop_close(loop) == -1);
	io->destroy(io);
	assert(protobuf_c_grpc2_poll_loop_close(loop) == 0);

	/* a system nobody took is freed by one release */
	loop = protobuf_c_grpc2_poll_loop_new();
	io = new_io(loop);
	assert(protobuf_c_grpc2_poll_loop_close(loop) == -1);
	io->destroy(io);
	assert(protobuf_c_grpc2_poll_loop_close(loop) == 0);
	puts("poll: ok");
	return 0;
}
