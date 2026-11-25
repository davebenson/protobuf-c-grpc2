/*
 * protobuf-c-grpc2: gRPC over HTTP/2 for protobuf-c, built on nghttp2-v2.
 *
 * The API is "sans-I/O": the application owns the sockets (and TLS). It feeds
 * bytes it received to protobuf_c_grpc2_conn_read() and asks protobuf_c_grpc2_conn_write() for bytes
 * to transmit. Servers and clients are both described using the standard
 * ProtobufCService interface, including the streaming extensions of
 * ProtobufCService::invoke (see protobuf-c.h).
 *
 * Nothing in this library is thread safe: a connection, and every call made
 * through it, must be used from one thread (or with external locking).
 */
#ifndef PROTOBUF_C_GRPC2_H
#define PROTOBUF_C_GRPC2_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <protobuf-c/protobuf-c.h>

#include "protobuf-c-grpc2-io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** gRPC status codes. */
enum {
	PROTOBUF_C_GRPC2_OK = 0,
	PROTOBUF_C_GRPC2_CANCELLED = 1,
	PROTOBUF_C_GRPC2_UNKNOWN = 2,
	PROTOBUF_C_GRPC2_INVALID_ARGUMENT = 3,
	PROTOBUF_C_GRPC2_DEADLINE_EXCEEDED = 4,
	PROTOBUF_C_GRPC2_NOT_FOUND = 5,
	PROTOBUF_C_GRPC2_ALREADY_EXISTS = 6,
	PROTOBUF_C_GRPC2_PERMISSION_DENIED = 7,
	PROTOBUF_C_GRPC2_RESOURCE_EXHAUSTED = 8,
	PROTOBUF_C_GRPC2_FAILED_PRECONDITION = 9,
	PROTOBUF_C_GRPC2_ABORTED = 10,
	PROTOBUF_C_GRPC2_OUT_OF_RANGE = 11,
	PROTOBUF_C_GRPC2_UNIMPLEMENTED = 12,
	PROTOBUF_C_GRPC2_INTERNAL = 13,
	PROTOBUF_C_GRPC2_UNAVAILABLE = 14,
	PROTOBUF_C_GRPC2_DATA_LOSS = 15,
	PROTOBUF_C_GRPC2_UNAUTHENTICATED = 16
};

/** Upper-case name of a status code, e.g. "NOT_FOUND". */
const char *protobuf_c_grpc2_status_name(int status);

/** A set of services that connections can serve. */
typedef struct ProtobufCGrpc2Server ProtobufCGrpc2Server;
/** One HTTP/2 connection, either the client or the server end. */
typedef struct ProtobufCGrpc2Conn ProtobufCGrpc2Conn;

/**
 * Called when a call finishes, on the client and the server alike. This is
 * how errors are reported for calls whose closure only sees a NULL message.
 * `message` may be NULL. `method` is NULL on the server when the request
 * named no known method (or was not gRPC).
 */
typedef void (*ProtobufCGrpc2CallDoneFn)(const ProtobufCMethodDescriptor *method,
					 int status, const char *message,
					 void *user_data);

/*
 * About all the options structs in this library: set `struct_size` to
 * sizeof the struct (the *_INIT macros do this) and leave every other field
 * you do not use zeroed. The library reads only as many bytes as
 * `struct_size` says, so a program built against an older version of a
 * struct keeps working when fields are added at its end. A struct with
 * `struct_size` 0 is rejected.
 */

/** Options for a connection; see the note above about `struct_size`. */
typedef struct ProtobufCGrpc2ConnOptions {
	/** sizeof(ProtobufCGrpc2ConnOptions). */
	size_t struct_size;
	/** Largest accepted message in bytes; 0 means 4 MiB. */
	size_t max_message_size;
	/** Client only: value of the `:authority` header; default "localhost". */
	const char *authority;
	/** Client only: `:scheme` header, "http" (default) or "https". */
	const char *scheme;
	/**
	 * Client: the timeout (ms, 0 = none) that service handles start with;
	 * see protobuf_c_grpc2_client_service_set_timeout_ms().
	 * Server: the deadline (ms, 0 = none) applied to calls whose client
	 * sent no `grpc-timeout`.
	 */
	uint64_t default_timeout_ms;
	/**
	 * Server: the longest deadline (ms, 0 = unlimited) a client may ask
	 * for; longer `grpc-timeout` values are cut down to this. Calls with
	 * no deadline are not affected (use default_timeout_ms for those).
	 */
	uint64_t max_timeout_ms;
	/** Optional; see ProtobufCGrpc2CallDoneFn. */
	ProtobufCGrpc2CallDoneFn on_call_done;
	void *user_data;
} ProtobufCGrpc2ConnOptions;

#define PROTOBUF_C_GRPC2_CONN_OPTIONS_INIT \
	{ sizeof(ProtobufCGrpc2ConnOptions) }

/*
 * The server type serves two uses. Without I/O components (options NULL,
 * or no `io`) it is just a registry of services that you attach to
 * connections you drive yourself. With an I/O system (see
 * ProtobufCGrpc2ServerOptions below) it also listens and runs its own
 * connections.
 */
typedef struct ProtobufCGrpc2ServerOptions ProtobufCGrpc2ServerOptions;

/**
 * Create a server from `options` (which may be NULL for an empty registry
 * with default settings). Services are not owned by the server; they must
 * outlive it. Returns NULL on failure, and then nothing has been taken. On
 * success the server takes its own references to `io` and `secure_transport`
 * (see ProtobufCGrpc2IoSystem::ref).
 */
ProtobufCGrpc2Server *protobuf_c_grpc2_server_new(
	const ProtobufCGrpc2ServerOptions *options);
void protobuf_c_grpc2_server_free(ProtobufCGrpc2Server *server);

/**
 * Register a service, which is looked up by its descriptor's full name.
 * Returns 0, or -1 if the descriptor has no name (CODE_SIZE) or the name is
 * already registered.
 */
int protobuf_c_grpc2_server_add_service(ProtobufCGrpc2Server *server, ProtobufCService *service);

/** Create the server end of a connection. Returns NULL on failure. */
ProtobufCGrpc2Conn *protobuf_c_grpc2_conn_server_new(ProtobufCGrpc2Server *server,
				 const ProtobufCGrpc2ConnOptions *options);

/** Create the client end of a connection. Returns NULL on failure. */
ProtobufCGrpc2Conn *protobuf_c_grpc2_conn_client_new(const ProtobufCGrpc2ConnOptions *options);

/**
 * Free a connection. Pending client calls are completed with
 * PROTOBUF_C_GRPC2_UNAVAILABLE and pending server input streams are destroyed. A server
 * handler that still holds a response closure may keep calling it; the calls
 * are ignored, and the final one (NULL message, or the first response of a
 * non-output-streaming method) releases the call's memory. Handlers that
 * never finish their closure leak that memory.
 * ProtobufCInputStream objects still held by the application stay valid
 * (sending on them fails) until the application destroys them.
 */
void protobuf_c_grpc2_conn_free(ProtobufCGrpc2Conn *conn);

/**
 * Process bytes received from the peer. Application callbacks and service
 * handlers run from inside this call. Returns 0, or a negative value when
 * the connection is broken and must be closed.
 */
int protobuf_c_grpc2_conn_read(ProtobufCGrpc2Conn *conn, const uint8_t *data, size_t len);

/**
 * Produce bytes to send to the peer, at most `len` (16384 is a good size).
 * Returns the number of bytes written (0 when there is nothing to send, so
 * call it repeatedly until it returns 0) or a negative value when the
 * connection must be closed.
 */
ssize_t protobuf_c_grpc2_conn_write(ProtobufCGrpc2Conn *conn, uint8_t *buf, size_t len);

/**
 * Absolute time (CLOCK_MONOTONIC ns) at which
 * protobuf_c_grpc2_conn_handle_expiry() must next be called, or UINT64_MAX
 * for none. Covers protocol timers and the deadlines of calls.
 */
uint64_t protobuf_c_grpc2_conn_get_expiry(const ProtobufCGrpc2Conn *conn);
/**
 * Nanoseconds until protobuf_c_grpc2_conn_handle_expiry() must be called: 0 if that is
 * already due, or -1 if there is no pending timer. Use this (rather than
 * protobuf_c_grpc2_conn_get_expiry()) to arm an event loop timer, since the clock
 * behind the absolute time may differ from the loop's.
 */
int64_t protobuf_c_grpc2_conn_get_timeout_ns(const ProtobufCGrpc2Conn *conn);
/**
 * Handle expired timers, including call deadlines: a client call past its
 * deadline is cancelled (RST_STREAM CANCEL) and completes with
 * PROTOBUF_C_GRPC2_DEADLINE_EXCEEDED; a server call is answered with
 * DEADLINE_EXCEEDED. (The service handler is not told: it can keep running
 * and its later responses are ignored.) Returns a negative value if the
 * connection must be closed. Must not free the connection from the
 * callbacks it triggers.
 */
int protobuf_c_grpc2_conn_handle_expiry(ProtobufCGrpc2Conn *conn);

/**
 * Set the timeout for calls made through this client service handle from now
 * on (0 = no deadline). The deadline runs from the moment the call is
 * started and covers the whole call, streaming included; it is also sent to
 * the server as `grpc-timeout`. Handles are cheap and independent, so for
 * different timeouts, get one handle per timeout from
 * protobuf_c_grpc2_conn_client_service(). Returns -1 if `service` is not a
 * client service of this library.
 */
int protobuf_c_grpc2_client_service_set_timeout_ms(ProtobufCService *service,
						   uint64_t timeout_ms);

/**
 * Get a client-side ProtobufCService for the given service descriptor. Use
 * it with the generated caller functions, or with its `invoke` directly.
 * Streaming flags in the method index are honoured; for a method with
 * INPUT_STREAMING the returned ProtobufCInputStream must be ended
 * (send_input(NULL)) and destroyed by the caller. NULL is returned (and the
 * closure is called with NULL) if the call cannot be started.
 *
 * The service is owned by the connection and is freed with it; calling
 * protobuf_c_service_destroy() on it is a no-op.
 */
ProtobufCService *protobuf_c_grpc2_conn_client_service(
	ProtobufCGrpc2Conn *conn,
	const ProtobufCServiceDescriptor *descriptor);

/* ------------------------------------------------------------------ */
/* Servers and clients that run their own connections */

/**
 * Options for protobuf_c_grpc2_server_new(). Everything is optional except
 * `struct_size` (see the note about options structs above), but a server
 * that listens needs `io`.
 *
 *   ProtobufCGrpc2ServerOptions o = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
 *   ProtobufCService *services[] = { &my_service.base };
 *   o.services = services;
 *   o.n_services = 1;
 *   o.io = protobuf_c_grpc2_io_system_new_uv(loop);            // -uv library
 *   o.secure_transport = protobuf_c_grpc2_secure_transport_system_new_openssl(ctx);
 *   ProtobufCGrpc2Server *server = protobuf_c_grpc2_server_new(&o);
 *   protobuf_c_grpc2_server_listen(server, "0.0.0.0", 50051);
 */
struct ProtobufCGrpc2ServerOptions {
	/** sizeof(ProtobufCGrpc2ServerOptions). */
	size_t struct_size;
	/** The services to serve, looked up by full name; the array is copied
	 *  but the services are not and must outlive the server. */
	ProtobufCService *const *services;
	size_t n_services;
	/**
	 * The event loop (see protobuf-c-grpc2-io.h). Ownership passes to the
	 * server on success, which calls its `destroy` at the end. Without it
	 * the server cannot listen.
	 */
	ProtobufCGrpc2IoSystem *io;
	/**
	 * Optional TLS. With it, connections use TLS and must negotiate ALPN
	 * "h2"; without it they are cleartext HTTP/2 (h2c), which is right
	 * behind a TLS-terminating proxy. Ownership as for `io`.
	 */
	ProtobufCGrpc2SecureTransportSystem *secure_transport;
	/** Largest accepted message in bytes; 0 means 4 MiB. */
	size_t max_message_size;
	/** Deadline (ms) for calls whose client sent no `grpc-timeout`; 0 =
	 *  none. */
	uint64_t default_timeout_ms;
	/** The longest deadline (ms) a client may ask for; 0 = unlimited. */
	uint64_t max_timeout_ms;
	/** Optional; see ProtobufCGrpc2CallDoneFn. */
	ProtobufCGrpc2CallDoneFn on_call_done;
	void *user_data;
};

#define PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT \
	{ sizeof(ProtobufCGrpc2ServerOptions) }

/** Start accepting connections; the server must have an I/O system. May be
 *  called more than once. Returns 0, or -1. */
int protobuf_c_grpc2_server_listen(ProtobufCGrpc2Server *server,
				   const char *host, unsigned port);

/*
 * protobuf_c_grpc2_server_free() on a server with an I/O system closes the
 * listeners and connections; the memory goes once the event loop has
 * finished closing them.
 */

/*
 * A client is a "channel" to one server (host and port). It connects lazily
 * on the first call and queues calls until the connection is ready. If the
 * connection is lost, calls in flight fail with UNAVAILABLE and the next
 * call reconnects. If a connection attempt fails (or the connection is
 * lost before the server ever answered, e.g. a failed TLS handshake), the
 * client backs off: calls made during the backoff fail at once with
 * UNAVAILABLE, and the delay grows exponentially up to a maximum. There is
 * one connection per client.
 */
typedef struct ProtobufCGrpc2Client ProtobufCGrpc2Client;

/**
 * Options for protobuf_c_grpc2_client_new(); `struct_size`, `host`, `port`
 * and `io` are required.
 *
 *   ProtobufCGrpc2ClientOptions o = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
 *   o.host = "example.com";
 *   o.port = 50051;
 *   o.io = protobuf_c_grpc2_io_system_new_uv(loop);
 *   o.secure_transport = protobuf_c_grpc2_secure_transport_system_new_openssl(ctx);
 *   ProtobufCGrpc2Client *client = protobuf_c_grpc2_client_new(&o);
 *   ProtobufCService *svc = protobuf_c_grpc2_client_get_service(client, &desc);
 */
typedef struct ProtobufCGrpc2ClientOptions {
	/** sizeof(ProtobufCGrpc2ClientOptions). */
	size_t struct_size;
	/** The server to talk to; `host` may be a name (copied). */
	const char *host;
	unsigned port;
	/** The name used for TLS (SNI and certificate verification); default:
	 *  `host`. */
	const char *server_name;
	/** Value of the `:authority` header; default "host:port". */
	const char *authority;
	/** The event loop; ownership passes to the client on success. */
	ProtobufCGrpc2IoSystem *io;
	/** Optional TLS (the server must negotiate ALPN "h2"); ownership as
	 *  for `io`. Configure trust roots and verification on the TLS
	 *  library's context. */
	ProtobufCGrpc2SecureTransportSystem *secure_transport;
	/** Largest accepted message in bytes; 0 means 4 MiB. */
	size_t max_message_size;
	/** Timeout (ms, 0 = none) that service handles start with; see
	 *  protobuf_c_grpc2_client_service_set_timeout_ms(). */
	uint64_t default_timeout_ms;
	/** Reconnection backoff: first delay and maximum, in ms (0 = 1000 and
	 *  120000), growing by x1.6. */
	uint64_t initial_backoff_ms;
	uint64_t max_backoff_ms;
	/** How long a connection attempt may take, in ms (0 = 20000). */
	uint64_t connect_timeout_ms;
	/** Optional; see ProtobufCGrpc2CallDoneFn. */
	ProtobufCGrpc2CallDoneFn on_call_done;
	void *user_data;
} ProtobufCGrpc2ClientOptions;

#define PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT \
	{ sizeof(ProtobufCGrpc2ClientOptions) }

/**
 * Create a client; no connection is made until the first call. Returns NULL
 * on failure, and then nothing has been taken. On success the client takes
 * its own references to `io` and `secure_transport`.
 */
ProtobufCGrpc2Client *protobuf_c_grpc2_client_new(
	const ProtobufCGrpc2ClientOptions *options);

/**
 * A ProtobufCService for `descriptor` that talks to the server through this
 * client. The handle stays valid across reconnects and is owned by the
 * client (do not free it). Use protobuf_c_grpc2_client_service_set_timeout_ms()
 * on it for deadlines. When a call cannot be started, the closure is called
 * with NULL and (for input streaming) the stream is NULL.
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

#endif /* PROTOBUF_C_GRPC2_H */
