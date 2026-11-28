/*
 * GLib main-loop adapter for protobuf-c-grpc2
 * (library: protobuf-c-grpc2-glib).
 *
 * Runs servers and clients on a GMainContext, so that a GLib or GTK program
 * can serve and call gRPC from its ordinary main-loop (g_main_loop_run(), the
 * GTK loop, g_main_context_iteration()...). All callbacks arrive from
 * whichever thread iterates the context, never from inside a call into this
 * library.
 *
 * The I/O system uses sources for file descriptors (g_source_add_unix_fd()),
 * timers and idle tasks, plus a source whose `prepare` function runs the
 * core's iteration hook (where it sends the output that callbacks have
 * produced). It needs GLib 2.36 or later; POSIX only.
 *
 * The I/O system supports thread safe clients (`post`).
 *
 * Host names given to `connect` are resolved with GResolver (GIO), without
 * blocking the loop, so the library needs GIO as well as GLib. The error code
 * `connect` reports for a failed lookup is -1000 - G_RESOLVER_ERROR_*.
 *
 * Limitation: `listen` takes address literals only, like the other adapters.
 */
#ifndef PROTOBUF_C_GRPC2_GLIB_H
#define PROTOBUF_C_GRPC2_GLIB_H

#include <glib.h>

#include "protobuf-c-grpc2.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * An I/O system that runs on `context`, or on g_main_context_default() if
 * `context` is NULL. The I/O system holds a reference to the context; the
 * context must be iterated by whoever wants the servers and clients to run,
 * and must not be destroyed before they are gone (the reference ensures it).
 * Use the I/O system, the servers and the clients only from the thread that
 * iterates the context.
 *
 * It is created floating: the first server or client to take it claims the
 * creation reference, and further ones share it. If you use it without one,
 * call `ref` and later `destroy`.
 *
 * Returns NULL on failure.
 */
ProtobufCGrpc2IoSystem *
protobuf_c_grpc2_io_system_new_glib(GMainContext *context);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_GLIB_H */
