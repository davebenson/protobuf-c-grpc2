#include <assert.h>
#include <string.h>

#include <openssl/pem.h>
#include <openssl/ssl.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2-openssl.h"
#include "tls-test-certs.h"
#include "tls-backend.h"

const char *
test_tls_name(void)
{
	return "openssl";
}

static X509 *
load_cert(const char *pem)
{
	BIO *b = BIO_new_mem_buf(pem, -1);
	X509 *x = PEM_read_bio_X509(b, NULL, NULL, NULL);

	BIO_free(b);
	assert(x != NULL);
	return x;
}

ProtobufCGrpc2SecureTransportSystem *
test_tls_new_server(void)
{
	SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
	BIO *b = BIO_new_mem_buf(test_server_key_pem, -1);
	EVP_PKEY *key = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
	X509 *cert = load_cert(test_server_cert_pem);
	ProtobufCGrpc2SecureTransportSystem *sys;
	ProtobufCGrpc2OpensslOptions so = PROTOBUF_C_GRPC2_OPENSSL_OPTIONS_INIT;

	assert(key != NULL);
	assert(SSL_CTX_use_certificate(ctx, cert) == 1);
	assert(SSL_CTX_use_PrivateKey(ctx, key) == 1);
	so.ctx = ctx;
	sys = protobuf_c_grpc2_secure_transport_system_new_openssl(&so);
	assert(sys != NULL);
	assert(SSL_CTX_get_min_proto_version(ctx) >= TLS1_2_VERSION);
	/* the system holds its own reference to the context */
	SSL_CTX_free(ctx);
	X509_free(cert);
	EVP_PKEY_free(key);
	BIO_free(b);
	return sys;
}

ProtobufCGrpc2SecureTransportSystem *
test_tls_new_client(int trusted)
{
	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
	X509 *cert = load_cert(trusted ? test_server_cert_pem
				       : test_other_cert_pem);
	ProtobufCGrpc2SecureTransportSystem *sys;
	ProtobufCGrpc2OpensslOptions so = PROTOBUF_C_GRPC2_OPENSSL_OPTIONS_INIT;

	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
	X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), cert);
	so.ctx = ctx;
	sys = protobuf_c_grpc2_secure_transport_system_new_openssl(&so);
	assert(sys != NULL);
	SSL_CTX_free(ctx);
	X509_free(cert);
	return sys;
}

ProtobufCGrpc2SecureTransportSystem *
test_tls_new_mtls_server(int client_auth, int identity_kind)
{
	SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
	BIO *b = BIO_new_mem_buf(test_server_key_pem, -1);
	EVP_PKEY *key = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
	X509 *cert = load_cert(test_server_cert_pem);
	X509 *clients[2];
	ProtobufCGrpc2OpensslOptions so = PROTOBUF_C_GRPC2_OPENSSL_OPTIONS_INIT;
	ProtobufCGrpc2SecureTransportSystem *sys;
	int i;

	assert(key != NULL);
	assert(SSL_CTX_use_certificate(ctx, cert) == 1);
	assert(SSL_CTX_use_PrivateKey(ctx, key) == 1);
	clients[0] = load_cert(test_client_A_cert_pem);
	clients[1] = load_cert(test_client_B_cert_pem);
	for (i = 0; i < 2; i++) {
		/* trusted, and listed as acceptable CAs in the request */
		assert(X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), clients[i]) == 1);
		assert(SSL_CTX_add_client_CA(ctx, clients[i]) == 1);
		X509_free(clients[i]);
	}
	so.ctx = ctx;
	so.client_auth = client_auth;
	so.peer_identity = identity_kind;
	sys = protobuf_c_grpc2_secure_transport_system_new_openssl(&so);
	assert(sys != NULL);
	SSL_CTX_free(ctx);
	X509_free(cert);
	EVP_PKEY_free(key);
	BIO_free(b);
	return sys;
}

ProtobufCGrpc2SecureTransportSystem *
test_tls_new_mtls_client(int which)
{
	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
	X509 *server = load_cert(test_server_cert_pem);
	ProtobufCGrpc2OpensslOptions so = PROTOBUF_C_GRPC2_OPENSSL_OPTIONS_INIT;
	ProtobufCGrpc2SecureTransportSystem *sys;

	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
	X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx), server);
	X509_free(server);
	if (which != 0) {
		BIO *b = BIO_new_mem_buf(which == 1 ? test_client_A_key_pem
					 : which == 2 ? test_client_B_key_pem
						      : test_client_C_key_pem, -1);
		EVP_PKEY *key = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
		X509 *cert = load_cert(which == 1 ? test_client_A_cert_pem
				       : which == 2 ? test_client_B_cert_pem
						    : test_client_C_cert_pem);

		assert(key != NULL);
		assert(SSL_CTX_use_certificate(ctx, cert) == 1);
		assert(SSL_CTX_use_PrivateKey(ctx, key) == 1);
		X509_free(cert);
		EVP_PKEY_free(key);
		BIO_free(b);
	}
	so.ctx = ctx;
	sys = protobuf_c_grpc2_secure_transport_system_new_openssl(&so);
	assert(sys != NULL);
	SSL_CTX_free(ctx);
	return sys;
}
