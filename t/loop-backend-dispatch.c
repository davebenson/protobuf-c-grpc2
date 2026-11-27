/*
 * The ProtobufCRPCDispatch backend of the loop tests. Built twice:
 *   loop-backend-dispatch.c        runs the loop with protobuf_c_rpc_dispatch_run()
 *   loop-backend-dispatch-embed.c  runs it from a main-loop of its own, driving
 *                                  protobuf_c_rpc_dispatch_dispatch() like an
 *                                  application that embeds the dispatch would
 */
#include <poll.h>
#include <stdlib.h>
#include <sys/time.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2-dispatch.h"
#include "loop-backend.h"

struct TestLoop {
	ProtobufCRPCDispatch *d;
};

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

const char *
test_backend_name(void)
{
#ifdef TEST_EMBEDDED_LOOP
	return "dispatch-embed";
#else
	return "dispatch";
#endif
}

TestLoop *
test_loop_new(void)
{
	TestLoop *l = calloc(1, sizeof(*l));

	if (l == NULL)
		return NULL;
	l->d = protobuf_c_rpc_dispatch_new(&test_allocator);
	if (l->d == NULL) {
		free(l);
		return NULL;
	}
	return l;
}

ProtobufCGrpc2IoSystem *
test_loop_new_io(TestLoop *l)
{
	return protobuf_c_grpc2_io_system_new_dispatch(l->d);
}

/* Something to wait for: a watched descriptor, a timer or an idle function. */
static int
alive(ProtobufCRPCDispatch *d)
{
	return d->n_notifies_desired != 0 || d->has_timeout || d->has_idle;
}

#ifdef TEST_EMBEDDED_LOOP
/* What an application with a main-loop of its own does with a dispatch: wait
 * on the descriptors it asks for, until its timeout, then hand over what
 * happened. */
static void
embedded_iteration(ProtobufCRPCDispatch *d)
{
	size_t n = d->n_notifies_desired, i, n_ready = 0;
	struct pollfd *pfds = calloc(n ? n : 1, sizeof(*pfds));
	ProtobufC_RPC_FDNotify *ready = calloc(n ? n : 1, sizeof(*ready));
	int timeout = -1;

	for (i = 0; i < n; i++) {
		pfds[i].fd = d->notifies_desired[i].fd;
		pfds[i].events =
			((d->notifies_desired[i].events & PROTOBUF_C_RPC_EVENT_READABLE) ? POLLIN : 0) |
			((d->notifies_desired[i].events & PROTOBUF_C_RPC_EVENT_WRITABLE) ? POLLOUT : 0);
	}
	if (d->has_idle) {
		timeout = 0;
	} else if (d->has_timeout) {
		struct timeval tv;
		long long ms;

		gettimeofday(&tv, NULL);
		ms = ((long long) d->timeout_secs - tv.tv_sec) * 1000 +
		     ((long long) d->timeout_usecs - tv.tv_usec + 999) / 1000;
		timeout = ms < 0 ? 0 : ms > 1000000 ? 1000000 : (int) ms;
	}
	if (poll(pfds, (nfds_t) n, timeout) > 0)
		for (i = 0; i < n; i++) {
			unsigned ev = 0;

			if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))
				ev |= PROTOBUF_C_RPC_EVENT_READABLE;
			if (pfds[i].revents & POLLOUT)
				ev |= PROTOBUF_C_RPC_EVENT_WRITABLE;
			if (ev != 0) {
				ready[n_ready].fd = pfds[i].fd;
				ready[n_ready++].events = ev;
			}
		}
	protobuf_c_rpc_dispatch_dispatch(d, n_ready, ready);
	free(pfds);
	free(ready);
}
#endif

void
test_loop_run(TestLoop *l)
{
#ifdef TEST_EMBEDDED_LOOP
	/* The first wait is not preceded by a dispatch: run the prepare
	 * functions by hand, as the header says. */
	protobuf_c_rpc_dispatch_run_prepares(l->d);
	while (alive(l->d))
		embedded_iteration(l->d);
#else
	while (alive(l->d))
		protobuf_c_rpc_dispatch_run(l->d);
#endif
}

int
test_loop_close(TestLoop *l)
{
	int leaked = alive(l->d);

	protobuf_c_rpc_dispatch_free(l->d);
	free(l);
	return leaked;
}
