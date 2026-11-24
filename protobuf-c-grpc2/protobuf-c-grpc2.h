/*
 * protobuf-c-grpc2: gRPC over HTTP/2 for protobuf-c, built on nghttp2-v2.
 *
 * The API is "sans-I/O": the application owns the sockets (and TLS). It feeds
 * bytes it received to grpc2_conn_read() and asks grpc2_conn_write() for bytes
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
	GRPC2_OK = 0,
	GRPC2_CANCELLED = 1,
	GRPC2_UNKNOWN = 2,
	GRPC2_INVALID_ARGUMENT = 3,
	GRPC2_DEADLINE_EXCEEDED = 4,
	GRPC2_NOT_FOUND = 5,
	GRPC2_ALREADY_EXISTS = 6,
	GRPC2_PERMISSION_DENIED = 7,
	GRPC2_RESOURCE_EXHAUSTED = 8,
	GRPC2_FAILED_PRECONDITION = 9,
	GRPC2_ABORTED = 10,
	GRPC2_OUT_OF_RANGE = 11,
	GRPC2_UNIMPLEMENTED = 12,
	GRPC2_INTERNAL = 13,
	GRPC2_UNAVAILABLE = 14,
	GRPC2_DATA_LOSS = 15,
	GRPC2_UNAUTHENTICATED = 16
};

/** Upper-case name of a status code, e.g. "NOT_FOUND". */
const char *grpc2_status_name(int status);

/** A set of services that connections can serve. */
typedef struct Grpc2Server Grpc2Server;
/** One HTTP/2 connection, either the client or the server end. */
typedef struct Grpc2Conn Grpc2Conn;

/** Options for a connection; zero-initialise and set what you need. */
typedef struct Grpc2ConnOptions {
	/** Largest accepted message in bytes; 0 means 4 MiB. */
	size_t max_message_size;
	/** Client only: value of the `:authority` header; default "localhost". */
	const char *authority;
	/** Client only: `:scheme` header, "http" (default) or "https". */
	const char *scheme;
	/**
	 * Optional. Called when a call finishes, on the client and the server
	 * alike. This is how errors are reported for calls whose closure only
	 * sees a NULL message. `message` may be NULL.
	 */
	void (*on_call_done)(const ProtobufCMethodDescriptor *method,
			     int status,
			     const char *message,
			     void *user_data);
	void *user_data;
} Grpc2ConnOptions;

/**
 * Create a server. Services are not owned by the server; they must outlive
 * every connection created from it.
 */
Grpc2Server *grpc2_server_new(void);
void grpc2_server_free(Grpc2Server *server);

/**
 * Register a service, which is looked up by its descriptor's full name.
 * Returns 0, or -1 if the descriptor has no name (CODE_SIZE) or the name is
 * already registered.
 */
int grpc2_server_add_service(Grpc2Server *server, ProtobufCService *service);

/** Create the server end of a connection. Returns NULL on failure. */
Grpc2Conn *grpc2_conn_server_new(Grpc2Server *server,
				 const Grpc2ConnOptions *options);

/** Create the client end of a connection. Returns NULL on failure. */
Grpc2Conn *grpc2_conn_client_new(const Grpc2ConnOptions *options);

/**
 * Free a connection. Pending client calls are completed with
 * GRPC2_UNAVAILABLE and pending server input streams are destroyed. A server
 * handler that still holds a response closure may keep calling it; the calls
 * are ignored, and the final one (NULL message, or the first response of a
 * non-output-streaming method) releases the call's memory. Handlers that
 * never finish their closure leak that memory.
 * ProtobufCInputStream objects still held by the application stay valid
 * (sending on them fails) until the application destroys them.
 */
void grpc2_conn_free(Grpc2Conn *conn);

/**
 * Process bytes received from the peer. Application callbacks and service
 * handlers run from inside this call. Returns 0, or a negative value when
 * the connection is broken and must be closed.
 */
int grpc2_conn_read(Grpc2Conn *conn, const uint8_t *data, size_t len);

/**
 * Produce bytes to send to the peer, at most `len` (16384 is a good size).
 * Returns the number of bytes written (0 when there is nothing to send, so
 * call it repeatedly until it returns 0) or a negative value when the
 * connection must be closed.
 */
ssize_t grpc2_conn_write(Grpc2Conn *conn, uint8_t *buf, size_t len);

/** Absolute time (CLOCK_MONOTONIC ns) at which grpc2_conn_handle_expiry()
 *  must next be called, or UINT64_MAX for none. */
uint64_t grpc2_conn_get_expiry(const Grpc2Conn *conn);
/** Handle expired timers. Returns a negative value if the connection must
 *  be closed. */
int grpc2_conn_handle_expiry(Grpc2Conn *conn);

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
ProtobufCService *grpc2_conn_client_service(
	Grpc2Conn *conn,
	const ProtobufCServiceDescriptor *descriptor);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_H */
