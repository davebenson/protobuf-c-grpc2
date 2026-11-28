/*
 * The GLib backend of the loop tests: a context of its own, iterated step by
 * step like an application that embeds GLib's main-loop in another loop.
 */
#include <stdlib.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2-glib.h"
#include "glib-iterate.h"
#include "loop-backend.h"

struct TestLoop {
	GMainContext *ctx;
};

const char *
test_backend_name(void)
{
	return "glib";
}

TestLoop *
test_loop_new(void)
{
	TestLoop *l = calloc(1, sizeof(*l));

	if (l == NULL)
		return NULL;
	l->ctx = g_main_context_new();
	return l;
}

ProtobufCGrpc2IoSystem *
test_loop_new_io(TestLoop *l)
{
	ProtobufCGrpc2GlibOptions o = PROTOBUF_C_GRPC2_GLIB_OPTIONS_INIT;

	o.context = l->ctx;
	return protobuf_c_grpc2_io_system_new_glib(&o);
}

void
test_loop_run(TestLoop *l)
{
	while (glib_iterate(l->ctx))
		;
}

int
test_loop_close(TestLoop *l)
{
	/* something left on it: a source of ours that still waits */
	int leaked = glib_iterate(l->ctx);

	g_main_context_unref(l->ctx);
	free(l);
	return leaked;
}
