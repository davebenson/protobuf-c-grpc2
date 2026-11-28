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
 * Nothing here is thread safe, with the single exception of `post`: every
 * call into the system and every callback is made from the event loop's
 * thread.
 */
#ifndef PROTOBUF_C_GRPC2_IO_H
#define PROTOBUF_C_GRPC2_IO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * For the constructors of adapters, which take an options struct each (see the
 * note about options structs in protobuf-c-grpc2.h): copy the caller's struct
 * into a zeroed, current-size one, reading only as many bytes as the caller's
 * `struct_size` (the first member) says it has, so that a program built against
 * an older version keeps working when fields are added. A NULL `src` gives
 * all defaults. Returns -1 if `src` has no usable `struct_size`.
 */
static inline int
protobuf_c_grpc2_options_copy(void *dst, size_t dst_size, const void *src)
{
	size_t src_size;

	memset(dst, 0, dst_size);
	*(size_t *) dst = dst_size;
	if (src == NULL)
		return 0;
	src_size = *(const size_t *) src;
	if (src_size < sizeof(size_t))
		return -1;
	memcpy(dst, src, src_size < dst_size ? src_size : dst_size);
	*(size_t *) dst = dst_size;
	return 0;
}

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

	/**
	 * Connect to host:port. `host` may be a name; the adapter resolves it
	 * and tries the addresses in order. Completion is always reported
	 * asynchronously (never from inside this call) as
	 * `connected(connected_data, stream, error)`: on success `stream` is a
	 * new stream and `error` is 0; otherwise `stream` is NULL and `error`
	 * is a nonzero, adapter specific code. `*handle` identifies the attempt
	 * until `connected` has been called. Returns 0, or -1 if it cannot
	 * even start (and then `connected` is not called).
	 */
	int (*connect)(ProtobufCGrpc2IoSystem *io, const char *host,
		       unsigned port,
		       void (*connected)(void *connected_data, void *stream,
					 int error),
		       void *connected_data, void **handle);
	/** Abandon a connection attempt that has not completed; `connected`
	 *  is then not called. */
	void (*cancel_connect)(ProtobufCGrpc2IoSystem *io, void *handle);

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

	/**
	 * Optional. Release one reference to the system, freeing it when the
	 * last one goes. Servers and clients call it when they are freed,
	 * after all their streams are closed.
	 */
	void (*destroy)(ProtobufCGrpc2IoSystem *io);
	/**
	 * Optional. Take one more reference. Servers and clients call it
	 * when they are created with the system as their `io` option, so a
	 * system can be shared freely. A system with a `ref` member is
	 * created "floating", as with g_object_ref_sink(): the first
	 * call claims its creation reference instead of adding one, so the
	 * creator never releases it, and a later call adds a reference. If
	 * nothing ever takes it (a system used only directly, for timers,
	 * say) the creator calls `ref` itself, and `destroy` when done. A
	 * system without `ref` is instead owned by the first server or
	 * client it is given to, which calls `destroy` on it.
	 */
	void (*ref)(ProtobufCGrpc2IoSystem *io);

	/**
	 * Optional, and the one member that may be called from any thread:
	 * arrange for `fn(data)` to be called from the event loop's thread,
	 * and wake the loop if it is waiting. `fn` is never called from inside
	 * this call (not even when it is made from the loop's thread), and
	 * posted functions run in the order they were posted. Returns 0, or -1
	 * (and then `fn` will not be called).
	 *
	 * The caller must keep the system alive until `fn` has run. Posted
	 * functions do not keep the loop running: a program that expects
	 * them must keep the loop alive itself, as it would to wait for a
	 * network event. A system without `post` cannot serve clients created
	 * with `thread_safe`.
	 */
	int (*post)(ProtobufCGrpc2IoSystem *io, void (*fn)(void *), void *data);
};

/* ------------------------------------------------------------------ */
/* Secure transport (TLS) */

/**
 * Whether a TLS server asks its clients for a certificate (mutual TLS). The
 * certificate must verify against the trust roots configured with the TLS
 * library (see the adapter headers); one that does not fails the handshake.
 */
typedef enum ProtobufCGrpc2ClientAuth {
	/** Do not ask: clients are anonymous at the TLS level (the default). */
	PROTOBUF_C_GRPC2_CLIENT_AUTH_NONE = 0,
	/** Ask; a client without a certificate is let in (and has no peer
	 *  identity), one with a bad certificate is not. An authenticator can
	 *  then decide about the anonymous ones. */
	PROTOBUF_C_GRPC2_CLIENT_AUTH_OPTIONAL = 1,
	/** Ask, and refuse clients without a valid certificate. */
	PROTOBUF_C_GRPC2_CLIENT_AUTH_REQUIRED = 2
} ProtobufCGrpc2ClientAuth;

/**
 * What the identity of a verified peer certificate is, as reported through
 * ProtobufCGrpc2SecureTransportVTable::peer_identity() and, on the server,
 * ProtobufCGrpc2CallInfo::peer.
 */
typedef enum ProtobufCGrpc2PeerIdentity {
	/** The first URI subject alternative name (a SPIFFE ID, say), else
	 *  the first DNS name, else the first e-mail address, else the
	 *  certificate's common name (the default). */
	PROTOBUF_C_GRPC2_PEER_IDENTITY_AUTO = 0,
	/** The subject's distinguished name, as in RFC 4514 ("CN=...,O=..."). */
	PROTOBUF_C_GRPC2_PEER_IDENTITY_SUBJECT = 1,
	/** The SHA-256 fingerprint of the certificate, in lower case hex:
	 *  identifies one certificate, for pinning. */
	PROTOBUF_C_GRPC2_PEER_IDENTITY_FINGERPRINT = 2
} ProtobufCGrpc2PeerIdentity;

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
	/**
	 * Optional (NULL: no identities). The identity of the peer, as
	 * established by its certificate, or NULL: the peer presented none, it
	 * was not verified (a client that does not verify its server, say), or
	 * none could be derived from it. Called once the handshake is complete
	 * (handshake_state is 1); the string stays valid until `destroy`. See
	 * ProtobufCGrpc2PeerIdentity for what the identity is.
	 */
	const char *(*peer_identity)(ProtobufCGrpc2SecureTransport *t);
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
	/**
	 * Optional. Release one reference to the system, freeing it when the
	 * last one goes. Servers and clients call it when they are freed.
	 */
	void (*destroy)(ProtobufCGrpc2SecureTransportSystem *sts);
	/**
	 * Optional. Take one reference. Servers and clients call it when
	 * they are created with the system as their `secure_transport`
	 * option, so one system can be shared. A system with a `ref` member
	 * is created "floating", as with g_object_ref_sink(): the first call
	 * claims its creation reference instead of adding one, so the
	 * creator never releases it, and a later call adds a reference. If
	 * nothing ever takes it, the creator frees it with `destroy`. A
	 * system without `ref` is instead owned by the first server or
	 * client it is given to, which calls `destroy` on it.
	 */
	void (*ref)(ProtobufCGrpc2SecureTransportSystem *sts);
};

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_IO_H */
