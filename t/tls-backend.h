/*
 * The TLS library under test. The endpoint test runs unchanged on every TLS
 * adapter: each test program is linked with exactly one of
 * tls-backend-openssl.c and tls-backend-gnutls.c. Both use the certificates
 * in tls-test-certs.h.
 */
#ifndef TLS_BACKEND_H
#define TLS_BACKEND_H

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"

const char *test_tls_name(void);
/* A new secure transport system for servers, using the server certificate. */
ProtobufCGrpc2SecureTransportSystem *test_tls_new_server(void);
/* A new system for clients that verify the server: they trust the server's
 * certificate (`trusted` nonzero) or only an unrelated one. */
ProtobufCGrpc2SecureTransportSystem *test_tls_new_client(int trusted);


/* ---- mutual TLS (certificates in tls-test-certs.h) ---- */

/* A server that asks for client certificates (a ProtobufCGrpc2ClientAuth),
 * trusts clients A and B but not C, and derives peer identities of the given
 * kind (a ProtobufCGrpc2PeerIdentity). */
ProtobufCGrpc2SecureTransportSystem *
test_tls_new_mtls_server(int client_auth, int identity_kind);
/* A client that trusts the server's certificate and presents client
 * certificate A, B or C (`which` 1, 2 or 3), or none (0). */
ProtobufCGrpc2SecureTransportSystem *test_tls_new_mtls_client(int which);

#endif
