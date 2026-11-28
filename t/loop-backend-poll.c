#include "protobuf-c-grpc2/protobuf-c-grpc2-poll.h"
#include "loop-backend.h"

struct TestLoop {
	ProtobufCGrpc2PollLoop *loop;
};

#include <stdlib.h>

const char *
test_backend_name(void)
{
	return "poll";
}

TestLoop *
test_loop_new(void)
{
	TestLoop *l = calloc(1, sizeof(*l));

	if (l == NULL)
		return NULL;
	l->loop = protobuf_c_grpc2_poll_loop_new();
	if (l->loop == NULL) {
		free(l);
		return NULL;
	}
	return l;
}

ProtobufCGrpc2IoSystem *
test_loop_new_io(TestLoop *l)
{
	ProtobufCGrpc2PollOptions o = PROTOBUF_C_GRPC2_POLL_OPTIONS_INIT;

	o.loop = l->loop;
	return protobuf_c_grpc2_io_system_new_poll(&o);
}

void
test_loop_run(TestLoop *l)
{
	protobuf_c_grpc2_poll_loop_run(l->loop);
}

int
test_loop_close(TestLoop *l)
{
	int rv = protobuf_c_grpc2_poll_loop_close(l->loop);	/* -1: leaked */

	if (rv == 0)
		free(l);
	return rv;
}
