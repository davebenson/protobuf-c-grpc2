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

/* ------------------------------------------------------------------ */
/* Clients */

/*
 * A client is a "channel" to one server (host and port). It connects lazily
 * on the first call and queues calls until the connection is ready. If the
 * connection is lost, calls in flight fail with UNAVAILABLE and the next
 * call reconnects. If a connection attempt fails (or the connection is
 * lost before the server ever answered), the client backs off: calls made
 * during the backoff fail at once with UNAVAILABLE, and the delay grows
 * exponentially up to a maximum.
 *
 *   ProtobufCGrpc2ClientBuilder *b = protobuf_c_grpc2_client_builder_new();
 *   protobuf_c_grpc2_client_builder_set_target(b, "example.com", 50051);
 *   protobuf_c_grpc2_client_builder_use_uv(b, loop);          // -uv library
 *   protobuf_c_grpc2_client_builder_use_openssl(b, ssl_ctx);  // optional TLS
 *   ProtobufCGrpc2Client *client = protobuf_c_grpc2_client_from_builder(b);
 *   protobuf_c_grpc2_client_builder_free(b);
 *   ProtobufCService *svc = protobuf_c_grpc2_client_get_service(client, &desc);
 *   ... call through svc with the generated callers ...
 *   protobuf_c_grpc2_client_free(client);
 */
typedef struct ProtobufCGrpc2ClientBuilder ProtobufCGrpc2ClientBuilder;
typedef struct ProtobufCGrpc2Client ProtobufCGrpc2Client;

ProtobufCGrpc2ClientBuilder *protobuf_c_grpc2_client_builder_new(void);
void protobuf_c_grpc2_client_builder_free(ProtobufCGrpc2ClientBuilder *builder);

/** The server to talk to. `host` may be a name. Returns 0, or -1. */
int protobuf_c_grpc2_client_builder_set_target(
	ProtobufCGrpc2ClientBuilder *builder, const char *host, unsigned port);

/** The name used for TLS (SNI and certificate verification); defaults to
 *  the target host. Returns 0, or -1. */
int protobuf_c_grpc2_client_builder_set_server_name(
	ProtobufCGrpc2ClientBuilder *builder, const char *server_name);

/** Connection options: default_timeout_ms, on_call_done, max_message_size,
 *  and the `:authority` (default "host:port"). Strings are copied. */
int protobuf_c_grpc2_client_builder_set_options(
	ProtobufCGrpc2ClientBuilder *builder,
	const ProtobufCGrpc2ConnOptions *options);

/** Reconnection backoff (default 1000 ms growing by x1.6 up to 120000 ms)
 *  and how long a connection attempt may take (default 20000 ms, 0 =
 *  unlimited). */
void protobuf_c_grpc2_client_builder_set_backoff(
	ProtobufCGrpc2ClientBuilder *builder, uint64_t initial_ms,
	uint64_t max_ms);
void protobuf_c_grpc2_client_builder_set_connect_timeout_ms(
	ProtobufCGrpc2ClientBuilder *builder, uint64_t ms);

/** As for the server builder: ownership passes to the builder. */
int protobuf_c_grpc2_client_builder_add_io_system(
	ProtobufCGrpc2ClientBuilder *builder, ProtobufCGrpc2IoSystem *io);
/** Optional: use TLS (the server must negotiate ALPN "h2"). */
int protobuf_c_grpc2_client_builder_add_secure_transport_system(
	ProtobufCGrpc2ClientBuilder *builder,
	ProtobufCGrpc2SecureTransportSystem *sts);

/**
 * Create the client; no connection is made until the first call. Needs a
 * target and an I/O system, else returns NULL. The components move to the
 * client, so the builder can be freed.
 */
ProtobufCGrpc2Client *protobuf_c_grpc2_client_from_builder(
	ProtobufCGrpc2ClientBuilder *builder);

/**
 * A ProtobufCService for `descriptor` that talks to the server through this
 * client. The handle stays valid across reconnects and is owned by the
 * client (do not free it). Use protobuf_c_grpc2_client_service_set_timeout_ms()
 * on it for deadlines. When a call cannot be started, the closure is called
 * with NULL and (for input streaming) the call is NULL.
 */
ProtobufCService *protobuf_c_grpc2_client_get_service(
	ProtobufCGrpc2Client *client,
	const ProtobufCServiceDescriptor *descriptor);

/**
 * Close the connection and free the client. Calls in flight complete with
 * UNAVAILABLE, which may be reported after this returns, while the event
 * loop finishes closing the connection; the service handles become invalid
 * immediately.
 */
void protobuf_c_grpc2_client_free(ProtobufCGrpc2Client *client);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_BUILDER_H */
