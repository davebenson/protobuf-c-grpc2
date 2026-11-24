/*
 * Plugin interfaces for protobuf-c-grpc2.
 *
 * The core library has no dependency on any event loop or TLS library.
 * Instead it talks to two small interfaces, implemented by adapter libraries
 * (protobuf-c-grpc2-uv, protobuf-c-grpc2-openssl) or by your own code:
 *
 *   ProtobufCGrpc2IoSystem               an event loop and its TCP streams
 *   ProtobufCGrpc2SecureTransportSystem  a TLS implementation
 *
 * Both are "derived" in the usual protobuf-c way: embed the base struct as
 * the first member of your own struct, and cast in the vtable functions.
 *
 * Like the rest of the library, nothing here is thread safe: every callback
 * must be made from the event loop's thread.
 */
#ifndef PROTOBUF_C_GRPC2_IO_H
#define PROTOBUF_C_GRPC2_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ProtobufCGrpc2IoSystem ProtobufCGrpc2IoSystem;
typedef struct ProtobufCGrpc2IoHandler ProtobufCGrpc2IoHandler;
typedef struct ProtobufCGrpc2SecureTransport ProtobufCGrpc2SecureTransport;
typedef struct ProtobufCGrpc2SecureTransportSystem
	ProtobufCGrpc2SecureTransportSystem;

/* ------------------------------------------------------------------ */
/* IO system */

/**
 * Events the core wants from one TCP stream. The core fills this in and
 * passes it to `stream_start`; the adapter copies it.
 */
struct ProtobufCGrpc2IoHandler {
	/** Bytes arrived. Only valid for the duration of the call. */
	void (*received)(void *data, const uint8_t *bytes, size_t len);
	/** The peer closed the stream, or it failed (`error` != 0). No more
	 *  `received` calls follow. The core will then call `stream_close`. */
	void (*eof)(void *data, int error);
	/** `stream_close` has completed; the adapter will not touch the
	 *  handler again and the core may free its state. */
	void (*closed)(void *data);
	void *data;
};

/**
 * An event loop with TCP. The streams, listeners and timers are opaque
 * pointers owned by the adapter.
 */
struct ProtobufCGrpc2IoSystem {
	/**
	 * Listen on host:port ("0.0.0.0", "::" or an IP literal). For every
	 * connection accepted, call `accepted(accepted_data, stream)`; the
	 * core then calls `stream_start` or `stream_close` on it. Stores the
	 * listener handle in `*listener`. Returns 0 or -1.
	 */
	int (*listen)(ProtobufCGrpc2IoSystem *io, const char *host,
		      unsigned port,
		      void (*accepted)(void *accepted_data, void *stream),
		      void *accepted_data, void **listener);
	/** Stop listening. May complete asynchronously; `accepted` is not
	 *  called after this returns. */
	void (*unlisten)(ProtobufCGrpc2IoSystem *io, void *listener);

	/** Begin reading from a stream and delivering events to `handler`. */
	int (*stream_start)(ProtobufCGrpc2IoSystem *io, void *stream,
			    const ProtobufCGrpc2IoHandler *handler);
	/** Queue `len` bytes for writing; the data is copied. 0 or -1. */
	int (*stream_write)(ProtobufCGrpc2IoSystem *io, void *stream,
			    const uint8_t *data, size_t len);
	/** Close the stream; `handler->closed` is called once finished
	 *  (never re-entrantly). A stream that was never started is simply
	 *  released, with no callback. */
	void (*stream_close)(ProtobufCGrpc2IoSystem *io, void *stream);

	/** A one-shot, restartable timer. `callback(data)` runs on expiry. */
	void *(*timer_new)(ProtobufCGrpc2IoSystem *io, void (*callback)(void *),
			   void *data);
	/** (Re)start the timer to fire in `ms` milliseconds. */
	void (*timer_start)(ProtobufCGrpc2IoSystem *io, void *timer,
			    uint64_t ms);
	void (*timer_stop)(ProtobufCGrpc2IoSystem *io, void *timer);
	/** Release the timer; its callback is not called afterwards. */
	void (*timer_free)(ProtobufCGrpc2IoSystem *io, void *timer);

	/**
	 * Arrange for `hook(data)` to be called every time the event loop is
	 * about to wait for events, after the callbacks (I/O and timers) of
	 * the current iteration have run, for as long as it is registered.
	 * This is where the core sends the output produced by callbacks and
	 * handlers, so it must run even when the output came from a timer:
	 * with libuv use a prepare handle, not a check handle, which would
	 * only run after the next wakeup. The hook must not by itself keep the
	 * loop alive. Returns a handle, or NULL on failure.
	 */
	void *(*add_iteration_hook)(ProtobufCGrpc2IoSystem *io,
				    void (*hook)(void *), void *data);
	void (*remove_iteration_hook)(ProtobufCGrpc2IoSystem *io, void *hook);

	/** Optional. Called when the server that owns this system is freed,
	 *  after all its streams are closed. */
	void (*destroy)(ProtobufCGrpc2IoSystem *io);
};

/* ------------------------------------------------------------------ */
/* Secure transport (TLS) */

/**
 * One TLS session, driven "sans-I/O" like the HTTP/2 engine: the core moves
 * bytes between the network and the session, and between the session and
 * HTTP/2. Derive from this struct; `vtable` must be the first member.
 */
typedef struct ProtobufCGrpc2SecureTransportVTable
	ProtobufCGrpc2SecureTransportVTable;
struct ProtobufCGrpc2SecureTransportVTable {
	/** Bytes received from the network. Returns 0, or -1 on a fatal TLS
	 *  error (the connection must then be closed). */
	int (*feed_ciphertext)(ProtobufCGrpc2SecureTransport *t,
			       const uint8_t *data, size_t len);
	/** Decrypted application data, if any. Returns the number of bytes
	 *  (0 if none is available now), or -1 when the session has failed
	 *  or been closed by the peer. */
	ssize_t (*read_plaintext)(ProtobufCGrpc2SecureTransport *t,
				  uint8_t *buf, size_t len);
	/** Encrypt application data. The transport buffers as much as
	 *  needed; returns `len` or -1. Only called once the handshake is
	 *  complete. */
	ssize_t (*write_plaintext)(ProtobufCGrpc2SecureTransport *t,
				   const uint8_t *data, size_t len);
	/** Bytes to send to the network (handshake messages and encrypted
	 *  data). Returns the number of bytes, 0 if none. */
	ssize_t (*read_ciphertext)(ProtobufCGrpc2SecureTransport *t,
				   uint8_t *buf, size_t len);
	/** 1 when the handshake is complete, 0 while in progress, -1 if it
	 *  failed. */
	int (*handshake_state)(ProtobufCGrpc2SecureTransport *t);
	/** The negotiated ALPN protocol (e.g. "h2"), or NULL. */
	const char *(*alpn)(ProtobufCGrpc2SecureTransport *t);
	void (*destroy)(ProtobufCGrpc2SecureTransport *t);
};

struct ProtobufCGrpc2SecureTransport {
	const ProtobufCGrpc2SecureTransportVTable *vtable;
};

/** A factory of TLS sessions: typically wraps one TLS context. */
struct ProtobufCGrpc2SecureTransportSystem {
	/** A session for an accepted connection. NULL on failure. */
	ProtobufCGrpc2SecureTransport *(*new_server)(
		ProtobufCGrpc2SecureTransportSystem *sts);
	/** A session for an outgoing connection to `server_name` (used for
	 *  SNI and certificate checks). May be NULL if the implementation
	 *  only supports servers. */
	ProtobufCGrpc2SecureTransport *(*new_client)(
		ProtobufCGrpc2SecureTransportSystem *sts,
		const char *server_name);
	/** Optional. Called when the owner is freed. */
	void (*destroy)(ProtobufCGrpc2SecureTransportSystem *sts);
};

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_IO_H */
