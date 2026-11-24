/*
 * libuv adapter for protobuf-c-grpc2 (library: protobuf-c-grpc2-uv).
 */
#ifndef PROTOBUF_C_GRPC2_UV_H
#define PROTOBUF_C_GRPC2_UV_H

#include <uv.h>

#include "protobuf-c-grpc2-builder.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * An I/O system that runs on `loop`. Free it with its `destroy` member, or
 * hand it to a builder (which takes ownership). The loop must outlive it.
 */
ProtobufCGrpc2IoSystem *protobuf_c_grpc2_io_system_new_uv(uv_loop_t *loop);

/** Shorthand for add_io_system(builder, protobuf_c_grpc2_io_system_new_uv(loop)). */
int protobuf_c_grpc2_server_builder_use_uv(ProtobufCGrpc2ServerBuilder *builder,
					   uv_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_UV_H */
