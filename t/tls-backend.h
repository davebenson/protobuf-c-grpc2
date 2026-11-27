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

#endif
