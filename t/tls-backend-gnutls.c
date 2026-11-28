#include <assert.h>
#include <string.h>

#include <gnutls/gnutls.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2-gnutls.h"
#include "tls-test-certs.h"
#include "tls-backend.h"

const char *
test_tls_name(void)
{
	return "gnutls";
}

static gnutls_datum_t
datum(const char *pem)
{
	gnutls_datum_t d;

	d.data = (unsigned char *) pem;
	d.size = (unsigned) strlen(pem);
	return d;
}

ProtobufCGrpc2SecureTransportSystem *
test_tls_new_server(void)
{
	gnutls_certificate_credentials_t cred;
	gnutls_datum_t cert = datum(test_server_cert_pem);
	gnutls_datum_t key = datum(test_server_key_pem);
	ProtobufCGrpc2GnutlsOptions o = PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT;
	ProtobufCGrpc2SecureTransportSystem *sys;

	assert(gnutls_certificate_allocate_credentials(&cred) == 0);
	assert(gnutls_certificate_set_x509_key_mem(cred, &cert, &key,
						   GNUTLS_X509_FMT_PEM) == 0);
	o.credentials = cred;
	sys = protobuf_c_grpc2_secure_transport_system_new_gnutls(&o);
	assert(sys != NULL);	/* owns the credentials now */
	return sys;
}

ProtobufCGrpc2SecureTransportSystem *
test_tls_new_client(int trusted)
{
	gnutls_certificate_credentials_t cred;
	gnutls_datum_t cert = datum(trusted ? test_server_cert_pem
					    : test_other_cert_pem);
	ProtobufCGrpc2GnutlsOptions o = PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT;
	ProtobufCGrpc2SecureTransportSystem *sys;

	assert(gnutls_certificate_allocate_credentials(&cred) == 0);
	assert(gnutls_certificate_set_x509_trust_mem(cred, &cert,
						     GNUTLS_X509_FMT_PEM) == 1);
	o.credentials = cred;
	sys = protobuf_c_grpc2_secure_transport_system_new_gnutls(&o);
	assert(sys != NULL);
	return sys;
}

ProtobufCGrpc2SecureTransportSystem *
test_tls_new_mtls_server(int client_auth, int identity_kind)
{
	gnutls_certificate_credentials_t cred;
	gnutls_datum_t cert = datum(test_server_cert_pem);
	gnutls_datum_t key = datum(test_server_key_pem);
	gnutls_datum_t a = datum(test_client_A_cert_pem);
	gnutls_datum_t b = datum(test_client_B_cert_pem);
	ProtobufCGrpc2GnutlsOptions o = PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT;
	ProtobufCGrpc2SecureTransportSystem *sys;

	assert(gnutls_certificate_allocate_credentials(&cred) == 0);
	assert(gnutls_certificate_set_x509_key_mem(cred, &cert, &key,
						   GNUTLS_X509_FMT_PEM) == 0);
	assert(gnutls_certificate_set_x509_trust_mem(cred, &a, GNUTLS_X509_FMT_PEM) == 1);
	assert(gnutls_certificate_set_x509_trust_mem(cred, &b, GNUTLS_X509_FMT_PEM) == 1);
	o.credentials = cred;
	o.client_auth = client_auth;
	o.peer_identity = identity_kind;
	sys = protobuf_c_grpc2_secure_transport_system_new_gnutls(&o);
	assert(sys != NULL);
	return sys;
}

ProtobufCGrpc2SecureTransportSystem *
test_tls_new_mtls_client(int which)
{
	gnutls_certificate_credentials_t cred;
	gnutls_datum_t server = datum(test_server_cert_pem);
	ProtobufCGrpc2GnutlsOptions o = PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT;
	ProtobufCGrpc2SecureTransportSystem *sys;

	assert(gnutls_certificate_allocate_credentials(&cred) == 0);
	assert(gnutls_certificate_set_x509_trust_mem(cred, &server,
						     GNUTLS_X509_FMT_PEM) == 1);
	if (which != 0) {
		gnutls_datum_t cert = datum(which == 1 ? test_client_A_cert_pem
				      : which == 2 ? test_client_B_cert_pem
						   : test_client_C_cert_pem);
		gnutls_datum_t key = datum(which == 1 ? test_client_A_key_pem
				     : which == 2 ? test_client_B_key_pem
						  : test_client_C_key_pem);

		assert(gnutls_certificate_set_x509_key_mem(
			cred, &cert, &key, GNUTLS_X509_FMT_PEM) == 0);
	}
	o.credentials = cred;
	sys = protobuf_c_grpc2_secure_transport_system_new_gnutls(&o);
	assert(sys != NULL);
	return sys;
}
