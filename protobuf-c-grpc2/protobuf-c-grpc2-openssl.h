/*
 * OpenSSL adapter for protobuf-c-grpc2 (library: protobuf-c-grpc2-openssl).
 * Works with any I/O system: it uses memory BIOs, never sockets.
 */
#ifndef PROTOBUF_C_GRPC2_OPENSSL_H
#define PROTOBUF_C_GRPC2_OPENSSL_H

#include <openssl/ssl.h>

#include "protobuf-c-grpc2.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Options for protobuf_c_grpc2_secure_transport_system_new_openssl(). Like the
 * other options structs of this library, set `struct_size` to sizeof the
 * struct (the _INIT macro does) and leave the fields you do not use zeroed.
 *
 *   ProtobufCGrpc2OpensslOptions o = PROTOBUF_C_GRPC2_OPENSSL_OPTIONS_INIT;
 *   o.ctx = ctx;
 *   sts = protobuf_c_grpc2_secure_transport_system_new_openssl(&o);
 */
typedef struct ProtobufCGrpc2OpensslOptions {
	/** sizeof(ProtobufCGrpc2OpensslOptions). */
	size_t struct_size;
	/** Required: the context the sessions are created from. */
	SSL_CTX *ctx;
} ProtobufCGrpc2OpensslOptions;

#define PROTOBUF_C_GRPC2_OPENSSL_OPTIONS_INIT \
	{ sizeof(ProtobufCGrpc2OpensslOptions) }

/**
 * A secure transport system whose sessions are created from `options->ctx`
 * (the struct is not kept). It is
 * created floating: the first server or client to take it claims the creation
 * reference, and further ones share it. The
 * reference count of the context is incremented, so the caller may free its own
 * reference. Returns NULL on failure, and then does not take a reference.
 *
 * This modifies the context: it installs an ALPN selection callback that accepts
 * "h2" (replacing any you set), and raises the minimum protocol version to
 * TLS 1.2 if it was lower, as gRPC requires. Client sessions offer only "h2". Configure
 * certificates, keys, protocol versions and (for clients) trust roots and
 * verification on `ctx` as usual. Client sessions verify the peer against
 * `server_name` when the context has SSL_VERIFY_PEER set.
 */
ProtobufCGrpc2SecureTransportSystem *
protobuf_c_grpc2_secure_transport_system_new_openssl(
	const ProtobufCGrpc2OpensslOptions *options);



#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_OPENSSL_H */
