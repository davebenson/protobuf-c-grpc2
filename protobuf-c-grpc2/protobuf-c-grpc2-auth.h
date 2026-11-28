/*
 * Authentication for protobuf-c-grpc2: call credentials for clients and an
 * authenticator for servers. Both are plugin objects like the I/O and TLS
 * systems: embed the base struct as the first member of your own and fill in
 * the function pointers. The core only ever passes them what a call looks
 * like (ProtobufCGrpc2CallInfo) and, on the server, the request headers it
 * asked for; no message is involved, so the decision is made before any
 * request data is parsed.
 *
 * Both hooks are asynchronous: they report their result by calling the `done`
 * function they are given, which may be done before the hook returns (the
 * fast path, which costs nothing extra) or later, for example after fetching
 * or refreshing a token. `done` is called exactly once, always from the
 * thread that runs the event loop, and the strings and arrays passed to a hook
 * are valid only until it returns: copy what you need to keep. A plugin that
 * works in other threads must post its answer back to the loop (see
 * ProtobufCGrpc2IoSystem::post).
 *
 * Credentials and the contents of headers are secrets: the library never logs
 * them, and sends credential headers with HPACK's "never index" flag.
 */
#ifndef PROTOBUF_C_GRPC2_AUTH_H
#define PROTOBUF_C_GRPC2_AUTH_H

#include <stddef.h>

#include <protobuf-c/protobuf-c.h>

#include "protobuf-c-grpc2-io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One HTTP/2 header (a gRPC metadata entry). Names are lower case. */
typedef struct ProtobufCGrpc2Header {
	const char *name;
	const char *value;
} ProtobufCGrpc2Header;

/** What a call is, as told to the hooks. Valid only during the hook. */
typedef struct ProtobufCGrpc2CallInfo {
	/** sizeof(ProtobufCGrpc2CallInfo) in the library that made it: fields
	 *  are only ever added at the end, so read a field only if
	 *  struct_size covers it. */
	size_t struct_size;
	const ProtobufCServiceDescriptor *service;
	const ProtobufCMethodDescriptor *method;
	/** Server side: the identity of the TLS client if its certificate
	 *  was verified (see ProtobufCGrpc2ClientAuth and
	 *  ProtobufCGrpc2PeerIdentity), else NULL, as for a cleartext
	 *  connection or a client without a certificate. Always NULL on the
	 *  client side. */
	const char *peer;
} ProtobufCGrpc2CallInfo;

/* ------------------------------------------------------------------ */
/* Clients: call credentials */

/**
 * The result of ProtobufCGrpc2CallCredentials::get. `status` is
 * PROTOBUF_C_GRPC2_OK (0) with the `headers` to send with the call, or an
 * error status (typically UNAUTHENTICATED or UNAVAILABLE) and a `message`,
 * which fails the call. Header names must be lower case, must not start with
 * ':' or "grpc-", and are not to be content-type, te or user-agent; a bad
 * header fails the call with INTERNAL.
 */
typedef void (*ProtobufCGrpc2CredentialsDone)(
	void *done_data, int status, const char *message,
	const ProtobufCGrpc2Header *headers, size_t n_headers);

typedef struct ProtobufCGrpc2CallCredentials ProtobufCGrpc2CallCredentials;
struct ProtobufCGrpc2CallCredentials {
	/**
	 * Supply the headers for one call that is about to start (at most
	 * once per call; the call waits for the answer, and its deadline keeps
	 * running meanwhile). `io` is the client's I/O system, for timers and
	 * the like.
	 */
	void (*get)(ProtobufCGrpc2CallCredentials *self,
		    ProtobufCGrpc2IoSystem *io,
		    const ProtobufCGrpc2CallInfo *info,
		    ProtobufCGrpc2CredentialsDone done, void *done_data);
	/**
	 * Optional. Release one reference, freeing the object with the last.
	 * Called when the client is freed; requests that are still
	 * outstanding must still be answered (or dropped, which leaks the
	 * call's memory).
	 */
	void (*destroy)(ProtobufCGrpc2CallCredentials *self);
	/** Optional. Take one reference; floating, as for
	 *  ProtobufCGrpc2IoSystem::ref. */
	void (*ref)(ProtobufCGrpc2CallCredentials *self);
};

/* ------------------------------------------------------------------ */
/* Servers: authenticator */

/**
 * The verdict of ProtobufCGrpc2ServerAuth::check. PROTOBUF_C_GRPC2_OK lets
 * the call proceed; `principal` is then an optional identity (a user name, say)
 * that the handler can read with protobuf_c_grpc2_current_call_principal().
 * Any other status (UNAUTHENTICATED, PERMISSION_DENIED...) ends the call with
 * that status and `message`; the handler is never invoked.
 */
typedef void (*ProtobufCGrpc2AuthDone)(void *done_data, int status,
				       const char *message,
				       const char *principal);

typedef struct ProtobufCGrpc2ServerAuth ProtobufCGrpc2ServerAuth;
struct ProtobufCGrpc2ServerAuth {
	/**
	 * Which request headers `check` wants to see, by lower case name.
	 * NULL (with n_header_names 0) means just "authorization". Read when
	 * the server is created.
	 */
	const char *const *header_names;
	size_t n_header_names;
	/**
	 * Decide whether a call may proceed, once its request headers are in
	 * and its method is known (unknown methods are answered
	 * UNIMPLEMENTED without asking). `headers` holds the requested
	 * headers that the request had, possibly none. While the answer is
	 * outstanding the library holds back the request's data, which flow
	 * control limits to one window, and the call's deadline keeps running.
	 */
	void (*check)(ProtobufCGrpc2ServerAuth *self,
		      const ProtobufCGrpc2CallInfo *info,
		      const ProtobufCGrpc2Header *headers, size_t n_headers,
		      ProtobufCGrpc2AuthDone done, void *done_data);
	/** Optional; see ProtobufCGrpc2CallCredentials::destroy. */
	void (*destroy)(ProtobufCGrpc2ServerAuth *self);
	/** Optional; see ProtobufCGrpc2CallCredentials::ref. */
	void (*ref)(ProtobufCGrpc2ServerAuth *self);
};

/**
 * The principal that the authenticator gave the call whose handler is being
 * invoked, or NULL (no authenticator, or it gave none). Valid only during the
 * handler's `invoke` call, on the loop thread: copy it to keep it.
 */
const char *protobuf_c_grpc2_current_call_principal(void);

/* ------------------------------------------------------------------ */
/* Ready-made implementations (no dependencies) */

/** Options for protobuf_c_grpc2_call_credentials_new_bearer(). */
typedef struct ProtobufCGrpc2BearerCredentialsOptions {
	/** sizeof(ProtobufCGrpc2BearerCredentialsOptions). */
	size_t struct_size;
	/** Required: the token; copied. */
	const char *token;
} ProtobufCGrpc2BearerCredentialsOptions;

#define PROTOBUF_C_GRPC2_BEARER_CREDENTIALS_OPTIONS_INIT \
	{ sizeof(ProtobufCGrpc2BearerCredentialsOptions) }

/**
 * Credentials that send "authorization: Bearer <token>" with every call.
 * Created floating, like the I/O systems. Returns NULL on failure.
 */
ProtobufCGrpc2CallCredentials *protobuf_c_grpc2_call_credentials_new_bearer(
	const ProtobufCGrpc2BearerCredentialsOptions *options);

/**
 * Checks a bearer token. Called with the token from the request (the
 * "authorization" header without its "Bearer " prefix); answer through `done`,
 * now or later.
 */
typedef void (*ProtobufCGrpc2BearerVerifyFn)(
	void *user_data, const char *token, const ProtobufCGrpc2CallInfo *info,
	ProtobufCGrpc2AuthDone done, void *done_data);

/** Options for protobuf_c_grpc2_server_auth_new_bearer(). */
typedef struct ProtobufCGrpc2BearerAuthOptions {
	/** sizeof(ProtobufCGrpc2BearerAuthOptions). */
	size_t struct_size;
	/** Required. */
	ProtobufCGrpc2BearerVerifyFn verify;
	void *user_data;
	/** Optional: called with `user_data` when the authenticator is
	 *  destroyed. */
	void (*free_user_data)(void *user_data);
} ProtobufCGrpc2BearerAuthOptions;

#define PROTOBUF_C_GRPC2_BEARER_AUTH_OPTIONS_INIT \
	{ sizeof(ProtobufCGrpc2BearerAuthOptions) }

/**
 * An authenticator for bearer tokens: a call with no "authorization: Bearer
 * ..." header is answered UNAUTHENTICATED without calling `verify`, the others
 * are decided by it. Created floating. Returns NULL on failure.
 */
ProtobufCGrpc2ServerAuth *protobuf_c_grpc2_server_auth_new_bearer(
	const ProtobufCGrpc2BearerAuthOptions *options);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_AUTH_H */
