/*
 * ProtobufCDispatch adapter for protobuf-c-grpc2
 * (library: protobuf-c-grpc2-dispatch).
 *
 * Runs servers and clients on the ProtobufCRPCDispatch of protobuf-c-rpc, so
 * that a program that already has a dispatch for its protobuf-c-rpc services
 * can add gRPC to the same main-loop, or can be driven by it. Whoever runs the
 * dispatch (protobuf_c_rpc_dispatch_run(), or an application's own loop calling
 * protobuf_c_rpc_dispatch_dispatch()) runs the I/O system.
 *
 * The I/O system uses the dispatch's file-descriptor watches, timers, idle
 * functions (to report completions from the loop, never from inside a call)
 * and prepare functions (where the core sends the output that callbacks have
 * produced), so it needs a protobuf-c-rpc that has the latter.
 *
 * An application that drives the dispatch itself through
 * protobuf_c_rpc_dispatch_dispatch() gets the prepare functions after each
 * dispatch; it may call protobuf_c_rpc_dispatch_run_prepares() once before its
 * first wait so that output produced while setting up is sent too.
 *
 * The I/O system has no `post` function, so clients on it cannot be
 * `thread_safe`: a ProtobufCRPCDispatch has no way to be woken from another
 * thread.
 *
 * Limitations: host names given to `connect` are resolved with a blocking
 * getaddrinfo() call (numeric addresses never block); `listen` takes address
 * literals only, like the other adapters; POSIX only.
 */
#ifndef PROTOBUF_C_GRPC2_DISPATCH_H
#define PROTOBUF_C_GRPC2_DISPATCH_H

#include <protobuf-c-rpc/protobuf-c-rpc-dispatch.h>

#include "protobuf-c-grpc2.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Options for protobuf_c_grpc2_io_system_new_dispatch(). Like the other
 * options structs of this library, set `struct_size` to sizeof the struct (the
 * _INIT macro does) and leave the fields you do not use zeroed.
 *
 *   ProtobufCGrpc2DispatchOptions o = PROTOBUF_C_GRPC2_DISPATCH_OPTIONS_INIT;
 *   o.dispatch = d;                            // or leave NULL: the default
 *   io = protobuf_c_grpc2_io_system_new_dispatch(&o);
 */
typedef struct ProtobufCGrpc2DispatchOptions {
	/** sizeof(ProtobufCGrpc2DispatchOptions). */
	size_t struct_size;
	/** The dispatch to run on; NULL means protobuf_c_rpc_dispatch_default(). */
	ProtobufCRPCDispatch *dispatch;
} ProtobufCGrpc2DispatchOptions;

#define PROTOBUF_C_GRPC2_DISPATCH_OPTIONS_INIT \
	{ sizeof(ProtobufCGrpc2DispatchOptions) }

/**
 * An I/O system that runs on `options->dispatch` (`options` may be NULL, for
 * the default dispatch). The dispatch is not owned by the I/O system and must
 * outlive it and the servers and clients that use it.
 *
 * It is created floating: the first server or client to take it claims the
 * creation reference, and further ones share it. If you use it without one,
 * call `ref` and later `destroy`.
 *
 * Returns NULL on failure.
 */
ProtobufCGrpc2IoSystem *
protobuf_c_grpc2_io_system_new_dispatch(const ProtobufCGrpc2DispatchOptions *options);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_DISPATCH_H */
