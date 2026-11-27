/*
 * The OpenSSL and GnuTLS secure transports talk to each other: every pairing
 * of a client and a server library, with a trusted certificate (handshake,
 * ALPN, data both ways, a large transfer) and an untrusted one (the client
 * must refuse). The sessions are connected directly through their
 * ciphertext interfaces, so no event loop is involved.
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the two shims define the same functions: give each library its own names */
#define test_tls_name ossl_name
#define test_tls_new_server ossl_new_server
#define test_tls_new_client ossl_new_client
#include "tls-backend-openssl.c"
#undef test_tls_name
#undef test_tls_new_server
#undef test_tls_new_client
#define test_tls_name gtls_name
#define test_tls_new_server gtls_new_server
#define test_tls_new_client gtls_new_client
#include "tls-backend-gnutls.c"

typedef ProtobufCGrpc2SecureTransport Sess;

/* Move ciphertext both ways until nothing moves. */
static void
pump(Sess *c, Sess *s)
{
	int round;

	for (round = 0; round < 1000; round++) {
		uint8_t buf[4096];
		ssize_t n;
		int moved = 0;

		while ((n = c->vtable->read_ciphertext(c, buf, sizeof buf)) > 0) {
			moved = 1;
			s->vtable->feed_ciphertext(s, buf, (size_t) n);
		}
		while ((n = s->vtable->read_ciphertext(s, buf, sizeof buf)) > 0) {
			moved = 1;
			c->vtable->feed_ciphertext(c, buf, (size_t) n);
		}
		if (!moved)
			return;
	}
	assert(!"the handshake does not settle");
}

/* Send `len` bytes from `from` to `to` and check they arrive intact. */
static void
transfer(Sess *from, Sess *to, size_t len, Sess *c, Sess *s)
{
	uint8_t *data = malloc(len), *got = malloc(len);
	size_t i, have = 0;

	for (i = 0; i < len; i++)
		data[i] = (uint8_t) (i * 31 + len);
	assert(from->vtable->write_plaintext(from, data, len) == (ssize_t) len);
	while (have < len) {
		ssize_t n;

		pump(c, s);
		n = to->vtable->read_plaintext(to, got + have, len - have);
		assert(n >= 0);
		have += (size_t) n;
		if (n == 0 && have < len)
			pump(c, s);
	}
	assert(memcmp(data, got, len) == 0);
	free(data);
	free(got);
}

static void
pair(const char *what,
     ProtobufCGrpc2SecureTransportSystem *(*new_client)(int),
     ProtobufCGrpc2SecureTransportSystem *(*new_server)(void))
{
	ProtobufCGrpc2SecureTransportSystem *cs = new_client(1);
	ProtobufCGrpc2SecureTransportSystem *ss = new_server();
	ProtobufCGrpc2SecureTransportSystem *bad = new_client(0);
	Sess *c, *s;

	cs->ref(cs);
	ss->ref(ss);
	bad->ref(bad);

	/* trusted */
	c = cs->new_client(cs, "localhost");
	s = ss->new_server(ss);
	assert(c != NULL && s != NULL);
	pump(c, s);
	assert(c->vtable->handshake_state(c) == 1);
	assert(s->vtable->handshake_state(s) == 1);
	assert(strcmp(c->vtable->alpn(c), "h2") == 0);
	assert(strcmp(s->vtable->alpn(s), "h2") == 0);
	transfer(c, s, 5, c, s);
	transfer(s, c, 17, c, s);
	transfer(c, s, 300000, c, s);
	transfer(s, c, 300000, c, s);
	c->vtable->destroy(c);
	s->vtable->destroy(s);

	/* the certificate is for localhost: another name must not verify */
	c = cs->new_client(cs, "example.invalid");
	s = ss->new_server(ss);
	pump(c, s);
	assert(c->vtable->handshake_state(c) == -1);
	c->vtable->destroy(c);
	s->vtable->destroy(s);

	/* a client that does not trust the server's certificate refuses it */
	c = bad->new_client(bad, "localhost");
	s = ss->new_server(ss);
	pump(c, s);
	assert(c->vtable->handshake_state(c) == -1);
	assert(s->vtable->handshake_state(s) != 1 ||
	       s->vtable->read_plaintext(s, (uint8_t[1]) { 0 }, 1) < 0);
	c->vtable->destroy(c);
	s->vtable->destroy(s);

	cs->destroy(cs);
	ss->destroy(ss);
	bad->destroy(bad);
	printf("tls-interop: %s ok\n", what);
}

/* The options struct: a zero or too small struct_size, a missing or bad
 * policy, a struct from an older version (fields beyond struct_size are not
 * read), and a policy that restricts the protocol version. Run under a leak
 * checker: whatever fails must still free the credentials, except when the
 * options are unusable. */
static gnutls_certificate_credentials_t
server_creds(void)
{
	gnutls_certificate_credentials_t cred;
	gnutls_datum_t cert = { (unsigned char *) test_server_cert_pem,
				(unsigned) strlen(test_server_cert_pem) };
	gnutls_datum_t key = { (unsigned char *) test_server_key_pem,
			       (unsigned) strlen(test_server_key_pem) };

	assert(gnutls_certificate_allocate_credentials(&cred) == 0);
	assert(gnutls_certificate_set_x509_key_mem(cred, &cert, &key,
						   GNUTLS_X509_FMT_PEM) == 0);
	return cred;
}

static void
test_options(void)
{
	ProtobufCGrpc2GnutlsOptions o = PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT;
	ProtobufCGrpc2SecureTransportSystem *sys, *cs;
	gnutls_certificate_credentials_t cred;
	Sess *c, *s;

	/* unusable options touch nothing */
	assert(protobuf_c_grpc2_secure_transport_system_new_gnutls(NULL) == NULL);
	cred = server_creds();
	o.credentials = cred;
	o.struct_size = 0;
	assert(protobuf_c_grpc2_secure_transport_system_new_gnutls(&o) == NULL);
	o.struct_size = offsetof(ProtobufCGrpc2GnutlsOptions, credentials);
	assert(protobuf_c_grpc2_secure_transport_system_new_gnutls(&o) == NULL);
	o.struct_size = sizeof o;
	o.credentials = NULL;
	assert(protobuf_c_grpc2_secure_transport_system_new_gnutls(&o) == NULL);

	/* a bad policy fails, and frees the credentials */
	o.credentials = cred;
	o.policy = "NO:SUCH:+POLICY";
	assert(protobuf_c_grpc2_secure_transport_system_new_gnutls(&o) == NULL);

	/* an older struct (without `policy`) works, whatever follows it */
	cred = server_creds();
	o.credentials = cred;
	o.policy = "NO:SUCH:+POLICY";	/* beyond struct_size: not read */
	o.struct_size = offsetof(ProtobufCGrpc2GnutlsOptions, policy);
	sys = protobuf_c_grpc2_secure_transport_system_new_gnutls(&o);
	assert(sys != NULL);
	sys->destroy(sys);

	/* TLS 1.2 only, client and server */
	o = (ProtobufCGrpc2GnutlsOptions) PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT;
	o.credentials = server_creds();
	o.policy = "NORMAL:-VERS-ALL:+VERS-TLS1.2";
	sys = protobuf_c_grpc2_secure_transport_system_new_gnutls(&o);
	assert(sys != NULL);
	cs = gtls_new_client(1);
	sys->ref(sys);
	cs->ref(cs);
	c = cs->new_client(cs, "localhost");
	s = sys->new_server(sys);
	pump(c, s);
	assert(c->vtable->handshake_state(c) == 1);
	assert(s->vtable->handshake_state(s) == 1);
	c->vtable->destroy(c);
	s->vtable->destroy(s);
	cs->destroy(cs);
	sys->destroy(sys);

	/* TLS 1.2 only against a server that allows nothing else than 1.3 */
	o.credentials = server_creds();
	o.policy = "NORMAL:-VERS-ALL:+VERS-TLS1.3";
	sys = protobuf_c_grpc2_secure_transport_system_new_gnutls(&o);
	assert(sys != NULL);
	{
		ProtobufCGrpc2GnutlsOptions co = PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT;
		gnutls_certificate_credentials_t ccred;
		gnutls_datum_t cert = { (unsigned char *) test_server_cert_pem,
					(unsigned) strlen(test_server_cert_pem) };

		assert(gnutls_certificate_allocate_credentials(&ccred) == 0);
		assert(gnutls_certificate_set_x509_trust_mem(
			ccred, &cert, GNUTLS_X509_FMT_PEM) == 1);
		co.credentials = ccred;
		co.policy = "NORMAL:-VERS-ALL:+VERS-TLS1.2";
		cs = protobuf_c_grpc2_secure_transport_system_new_gnutls(&co);
		assert(cs != NULL);
	}
	sys->ref(sys);
	cs->ref(cs);
	c = cs->new_client(cs, "localhost");
	s = sys->new_server(sys);
	pump(c, s);
	assert(c->vtable->handshake_state(c) == -1);	/* no common version */
	c->vtable->destroy(c);
	s->vtable->destroy(s);
	cs->destroy(cs);
	sys->destroy(sys);
	puts("tls-interop: options ok");
}

int
main(void)
{
	test_options();
	pair("openssl client, openssl server", ossl_new_client, ossl_new_server);
	pair("gnutls client, gnutls server", gtls_new_client, gtls_new_server);
	pair("gnutls client, openssl server", gtls_new_client, ossl_new_server);
	pair("openssl client, gnutls server", ossl_new_client, gtls_new_server);
	puts("tls-interop: ok");
	return 0;
}
