/*
 * libuv adapter for protobuf-c-grpc2 (library: protobuf-c-grpc2-uv).
 */
#ifndef PROTOBUF_C_GRPC2_UV_H
#define PROTOBUF_C_GRPC2_UV_H

#include <uv.h>

#include "protobuf-c-grpc2.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Options for protobuf_c_grpc2_io_system_new_uv(). Like the other options
 * structs of this library, set `struct_size` to sizeof the struct (the _INIT
 * macro does) and leave the fields you do not use zeroed.
 *
 *   ProtobufCGrpc2UvOptions o = PROTOBUF_C_GRPC2_UV_OPTIONS_INIT;
 *   o.loop = loop;
 *   io = protobuf_c_grpc2_io_system_new_uv(&o);
 */
typedef struct ProtobufCGrpc2UvOptions {
	/** sizeof(ProtobufCGrpc2UvOptions). */
	size_t struct_size;
	/** The loop to run on; NULL means uv_default_loop(). */
	uv_loop_t *loop;
} ProtobufCGrpc2UvOptions;

#define PROTOBUF_C_GRPC2_UV_OPTIONS_INIT { sizeof(ProtobufCGrpc2UvOptions) }

/**
 * An I/O system that runs on `options->loop` (`options` may be NULL). It is created floating: the first
 * server or client to take it claims the creation reference, and further ones
 * share it. If you use it without one, call `ref` and later `destroy`.
 * The loop must outlive it, and must run once more after the last
 * reference is released so that the system's wake-up handle is closed (as
 * with any uv handle, before uv_loop_close()).
 *
 * The system supports thread safe clients: its `post` function uses a
 * uv_async_t, which does not keep the loop alive.
 */
ProtobufCGrpc2IoSystem *
protobuf_c_grpc2_io_system_new_uv(const ProtobufCGrpc2UvOptions *options);



#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_UV_H */
