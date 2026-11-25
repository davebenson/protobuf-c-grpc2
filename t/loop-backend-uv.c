#include <stdlib.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2-uv.h"
#include "loop-backend.h"

struct TestLoop {
	uv_loop_t loop;
};

const char *
test_backend_name(void)
{
	return "uv";
}

TestLoop *
test_loop_new(void)
{
	TestLoop *l = calloc(1, sizeof(*l));

	if (l != NULL && uv_loop_init(&l->loop) != 0) {
		free(l);
		return NULL;
	}
	return l;
}

ProtobufCGrpc2IoSystem *
test_loop_new_io(TestLoop *l)
{
	return protobuf_c_grpc2_io_system_new_uv(&l->loop);
}

void
test_loop_run(TestLoop *l)
{
	uv_run(&l->loop, UV_RUN_DEFAULT);
}

int
test_loop_close(TestLoop *l)
{
	int rv = uv_loop_close(&l->loop);	/* fails if any handle leaked */

	if (rv == 0)
		free(l);
	return rv;
}
