/*
 * GnuTLS adapter for protobuf-c-grpc2 (library: protobuf-c-grpc2-gnutls).
 * Works with any I/O system: it hands GnuTLS memory buffers, never sockets.
 */
#ifndef PROTOBUF_C_GRPC2_GNUTLS_H
#define PROTOBUF_C_GRPC2_GNUTLS_H

#include <stddef.h>

#include <gnutls/gnutls.h>

#include "protobuf-c-grpc2.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Options for protobuf_c_grpc2_secure_transport_system_new_gnutls().
 * Like the other options structs of this library, set `struct_size` to
 * sizeof the struct (the _INIT macro does) and leave the fields you do not
 * use zeroed: a program built against an older version of the struct keeps
 * working when fields are added at its end.
 *
 *   ProtobufCGrpc2GnutlsOptions o = PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT;
 *   o.credentials = cred;
 *   o.secure_transport = protobuf_c_grpc2_secure_transport_system_new_gnutls(&o);
 */
typedef struct ProtobufCGrpc2GnutlsOptions {
	/** sizeof(ProtobufCGrpc2GnutlsOptions). */
	size_t struct_size;
	/**
	 * Required. The certificates, keys and trust roots of the sessions:
	 * configure them as usual, with gnutls_certificate_set_x509_key_file(),
	 * _set_x509_trust_file(), _set_x509_system_trust() and so on, before
	 * creating the system.
	 *
	 * GnuTLS credentials are not reference counted (unlike an OpenSSL
	 * SSL_CTX), so the system takes ownership of them and frees them with
	 * its last reference. It does so when creating it fails as well, so
	 * the caller never frees them after the call. To make several systems,
	 * build credentials for each.
	 */
	gnutls_certificate_credentials_t credentials;
	/**
	 * Optional: a GnuTLS priority string, which chooses the protocol
	 * versions, ciphers and so on. NULL (the usual case) selects "NORMAL"
	 * restricted to TLS 1.2 and later, as gRPC requires; a string you
	 * pass is used as is, so it should not enable older versions.
	 */
	const char *policy;
	/**
	 * Servers: whether to ask clients for a certificate (mutual TLS). The
	 * certificate chain is verified against the trust roots in
	 * `credentials` (gnutls_certificate_set_x509_trust_file() and so on),
	 * and a client that fails is refused. Clients present the certificate
	 * and key in their `credentials` when a server asks
	 * (gnutls_certificate_set_x509_key_file()).
	 */
	int client_auth;		/* a ProtobufCGrpc2ClientAuth */
	/**
	 * What the identity of a verified peer certificate is (see
	 * ProtobufCGrpc2PeerIdentity). Both ends get one: a server for its
	 * client, a client for its server, when the certificate verified.
	 */
	int peer_identity;		/* a ProtobufCGrpc2PeerIdentity */
} ProtobufCGrpc2GnutlsOptions;

#define PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT \
	{ sizeof(ProtobufCGrpc2GnutlsOptions) }

/**
 * A secure transport system whose sessions are described by `options`
 * (see ProtobufCGrpc2GnutlsOptions; the struct is not kept).
 *
 * Sessions offer and accept only the ALPN protocol "h2". A client session
 * verifies the server's certificate chain against the trust roots in the
 * credentials, and its name against `server_name`: unlike with the OpenSSL
 * adapter, verification cannot be turned off, so a client needs trust roots
 * (or the server's own certificate) in its credentials. A server asks clients
 * for certificates only if `client_auth` says so.
 *
 * It is created floating: the first server or client to take it claims the
 * creation reference, and further ones share it.
 *
 * Returns NULL on failure. The credentials are then freed too, unless
 * `options` is unusable (NULL, a zero `struct_size`, or too small to contain
 * `credentials`), in which case nothing is touched.
 */
ProtobufCGrpc2SecureTransportSystem *
protobuf_c_grpc2_secure_transport_system_new_gnutls(
	const ProtobufCGrpc2GnutlsOptions *options);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_GNUTLS_H */
