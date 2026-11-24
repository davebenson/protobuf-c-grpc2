/*
 * OpenSSL adapter for protobuf-c-grpc2 (library: protobuf-c-grpc2-openssl).
 * Works with any I/O system: it uses memory BIOs, never sockets.
 */
#ifndef PROTOBUF_C_GRPC2_OPENSSL_H
#define PROTOBUF_C_GRPC2_OPENSSL_H

#include <openssl/ssl.h>

#include "protobuf-c-grpc2-builder.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A secure transport system whose sessions are created from `ctx`. The
 * reference count of `ctx` is incremented, so the caller may free its own
 * reference.
 *
 * This modifies `ctx`: it installs an ALPN selection callback that accepts
 * "h2" (replacing any you set), and raises the minimum protocol version to
 * TLS 1.2 if it was lower, as gRPC requires. Client sessions offer only "h2". Configure
 * certificates, keys, protocol versions and (for clients) trust roots and
 * verification on `ctx` as usual. Client sessions verify the peer against
 * `server_name` when the context has SSL_VERIFY_PEER set.
 */
ProtobufCGrpc2SecureTransportSystem *
protobuf_c_grpc2_secure_transport_system_new_openssl(SSL_CTX *ctx);

/** Shorthand for add_secure_transport_system(builder, ..._new_openssl(ctx)). */
int protobuf_c_grpc2_server_builder_use_openssl(
	ProtobufCGrpc2ServerBuilder *builder, SSL_CTX *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_OPENSSL_H */
