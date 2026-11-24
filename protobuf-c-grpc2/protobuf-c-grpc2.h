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

/** Options for a connection; zero-initialise and set what you need. */
typedef struct ProtobufCGrpc2ConnOptions {
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
	/**
	 * Optional. Called when a call finishes, on the client and the server
	 * alike. This is how errors are reported for calls whose closure only
	 * sees a NULL message. `message` may be NULL. `method` is NULL on the
	 * server when the request named no known method (or was not gRPC).
	 */
	void (*on_call_done)(const ProtobufCMethodDescriptor *method,
			     int status,
			     const char *message,
			     void *user_data);
	void *user_data;
} ProtobufCGrpc2ConnOptions;

/**
 * Create a server. Services are not owned by the server; they must outlive
 * every connection created from it.
 */
ProtobufCGrpc2Server *protobuf_c_grpc2_server_new(void);
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

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_H */
