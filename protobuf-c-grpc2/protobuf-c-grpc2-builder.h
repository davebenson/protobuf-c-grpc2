/*
 * Server builder: assembles a ready-to-run gRPC server from services plus
 * pluggable I/O and TLS implementations (see protobuf-c-grpc2-io.h). The
 * builder functions in this header have no dependencies beyond the core
 * library; adapter libraries provide convenience wrappers such as
 * protobuf_c_grpc2_server_builder_use_uv() and ..._use_openssl().
 *
 *   ProtobufCGrpc2ServerBuilder *b = protobuf_c_grpc2_server_builder_new();
 *   protobuf_c_grpc2_server_builder_add_service(b, &my_service.base);
 *   protobuf_c_grpc2_server_builder_use_uv(b, loop);        // -uv library
 *   protobuf_c_grpc2_server_builder_use_openssl(b, ssl_ctx); // optional TLS
 *   ProtobufCGrpc2Server *s = protobuf_c_grpc2_server_from_builder(b);
 *   protobuf_c_grpc2_server_builder_free(b);
 *   protobuf_c_grpc2_server_listen(s, "0.0.0.0", 50051);
 *   ... run the event loop ...
 *   protobuf_c_grpc2_server_free(s);
 */
#ifndef PROTOBUF_C_GRPC2_BUILDER_H
#define PROTOBUF_C_GRPC2_BUILDER_H

#include "protobuf-c-grpc2.h"
#include "protobuf-c-grpc2-io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ProtobufCGrpc2ServerBuilder ProtobufCGrpc2ServerBuilder;

ProtobufCGrpc2ServerBuilder *protobuf_c_grpc2_server_builder_new(void);

/** Free a builder. Components that were added but not yet handed to a
 *  server (see from_builder) are destroyed. */
void protobuf_c_grpc2_server_builder_free(ProtobufCGrpc2ServerBuilder *builder);

/** Serve `service`, which is not owned and must outlive the server.
 *  Returns 0, or -1 (no name, duplicate, out of memory). */
int protobuf_c_grpc2_server_builder_add_service(
	ProtobufCGrpc2ServerBuilder *builder, ProtobufCService *service);

/** Connection options (only max_message_size and on_call_done/user_data
 *  matter for servers). */
void protobuf_c_grpc2_server_builder_set_options(
	ProtobufCGrpc2ServerBuilder *builder,
	const ProtobufCGrpc2ConnOptions *options);

/**
 * Supply the event loop. Ownership of `io` passes to the builder (and then
 * to the server), which calls `io->destroy` at the end. Only one I/O system
 * can be added; returns -1 for a second one.
 */
int protobuf_c_grpc2_server_builder_add_io_system(
	ProtobufCGrpc2ServerBuilder *builder, ProtobufCGrpc2IoSystem *io);

/**
 * Optionally supply TLS; connections then use TLS (ALPN "h2" required).
 * Without it connections are cleartext HTTP/2 (h2c), which is right behind
 * a TLS-terminating proxy. Ownership as for add_io_system.
 */
int protobuf_c_grpc2_server_builder_add_secure_transport_system(
	ProtobufCGrpc2ServerBuilder *builder,
	ProtobufCGrpc2SecureTransportSystem *sts);

/**
 * Create the server. Requires an I/O system; returns NULL otherwise. The
 * I/O and TLS components move to the server, so the builder can be freed
 * (or reused for another server, which then needs new components).
 */
ProtobufCGrpc2Server *protobuf_c_grpc2_server_from_builder(
	ProtobufCGrpc2ServerBuilder *builder);

/** Start accepting connections. May be called more than once. 0 or -1. */
int protobuf_c_grpc2_server_listen(ProtobufCGrpc2Server *server,
				   const char *host, unsigned port);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_BUILDER_H */
