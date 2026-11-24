/* OpenSSL implementation of ProtobufCGrpc2SecureTransport. */
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>

#include "protobuf-c-grpc2-openssl.h"

typedef struct {
	ProtobufCGrpc2SecureTransportSystem base;	/* must be first */
	SSL_CTX *ctx;
} OsslSystem;

typedef struct {
	ProtobufCGrpc2SecureTransport base;		/* must be first */
	SSL *ssl;
	BIO *rbio;		/* ciphertext in (from the network) */
	BIO *wbio;		/* ciphertext out (to the network) */
	int state;		/* 0 handshaking, 1 done, -1 failed/closed */
	char alpn[16];
} OsslSession;

/* Server: choose "h2" from the client's offer. */
static int
alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen,
	    const unsigned char *in, unsigned inlen, void *arg)
{
	static const unsigned char h2[] = { 2, 'h', '2' };
	(void) ssl; (void) arg;

	if (SSL_select_next_proto((unsigned char **) out, outlen, h2, sizeof h2,
				  in, inlen) == OPENSSL_NPN_NEGOTIATED)
		return SSL_TLSEXT_ERR_OK;
	return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* Run the handshake as far as the data available allows. */
static void
advance(OsslSession *s)
{
	int rv, err;

	if (s->state != 0)
		return;
	rv = SSL_do_handshake(s->ssl);
	if (rv == 1) {
		s->state = 1;
		return;
	}
	err = SSL_get_error(s->ssl, rv);
	if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
		s->state = -1;
		ERR_clear_error();
	}
}

static int
sess_feed(ProtobufCGrpc2SecureTransport *t, const uint8_t *data, size_t len)
{
	OsslSession *s = (OsslSession *) t;

	if (s->state < 0)
		return -1;
	if (len > 0 && BIO_write(s->rbio, data, (int) len) != (int) len) {
		s->state = -1;
		return -1;
	}
	advance(s);
	return s->state < 0 ? -1 : 0;
}

static ssize_t
sess_read_plaintext(ProtobufCGrpc2SecureTransport *t, uint8_t *buf, size_t len)
{
	OsslSession *s = (OsslSession *) t;
	int n, err;

	if (s->state < 0)
		return -1;
	if (s->state == 0)
		return 0;
	n = SSL_read(s->ssl, buf, (int) len);
	if (n > 0)
		return n;
	err = SSL_get_error(s->ssl, n);
	if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
		return 0;
	/* close_notify or a real error */
	s->state = -1;
	ERR_clear_error();
	return -1;
}

static ssize_t
sess_write_plaintext(ProtobufCGrpc2SecureTransport *t, const uint8_t *data,
		     size_t len)
{
	OsslSession *s = (OsslSession *) t;
	size_t off = 0;

	if (s->state != 1)
		return -1;
	while (off < len) {
		int n = SSL_write(s->ssl, data + off, (int) (len - off));
		if (n <= 0) {
			s->state = -1;
			ERR_clear_error();
			return -1;
		}
		off += (size_t) n;
	}
	return (ssize_t) len;
}

static ssize_t
sess_read_ciphertext(ProtobufCGrpc2SecureTransport *t, uint8_t *buf, size_t len)
{
	OsslSession *s = (OsslSession *) t;
	int n = BIO_read(s->wbio, buf, (int) len);
	return n > 0 ? n : 0;
}

static int
sess_handshake_state(ProtobufCGrpc2SecureTransport *t)
{
	return ((OsslSession *) t)->state;
}

static const char *
sess_alpn(ProtobufCGrpc2SecureTransport *t)
{
	OsslSession *s = (OsslSession *) t;
	const unsigned char *p = NULL;
	unsigned n = 0;

	SSL_get0_alpn_selected(s->ssl, &p, &n);
	if (p == NULL || n == 0 || n >= sizeof s->alpn)
		return NULL;
	memcpy(s->alpn, p, n);
	s->alpn[n] = 0;
	return s->alpn;
}

static void
sess_destroy(ProtobufCGrpc2SecureTransport *t)
{
	OsslSession *s = (OsslSession *) t;
	SSL_free(s->ssl);		/* also frees the BIOs */
	free(s);
}

static const ProtobufCGrpc2SecureTransportVTable sess_vtable = {
	sess_feed, sess_read_plaintext, sess_write_plaintext,
	sess_read_ciphertext, sess_handshake_state, sess_alpn, sess_destroy
};

static OsslSession *
session_new(OsslSystem *sys)
{
	OsslSession *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return NULL;
	s->base.vtable = &sess_vtable;
	s->ssl = SSL_new(sys->ctx);
	s->rbio = BIO_new(BIO_s_mem());
	s->wbio = BIO_new(BIO_s_mem());
	if (s->ssl == NULL || s->rbio == NULL || s->wbio == NULL) {
		BIO_free(s->rbio);
		BIO_free(s->wbio);
		SSL_free(s->ssl);
		free(s);
		return NULL;
	}
	SSL_set_bio(s->ssl, s->rbio, s->wbio);	/* SSL owns both now */
	return s;
}

static ProtobufCGrpc2SecureTransport *
sys_new_server(ProtobufCGrpc2SecureTransportSystem *base)
{
	OsslSession *s = session_new((OsslSystem *) base);

	if (s == NULL)
		return NULL;
	SSL_set_accept_state(s->ssl);
	return &s->base;
}

static ProtobufCGrpc2SecureTransport *
sys_new_client(ProtobufCGrpc2SecureTransportSystem *base,
	       const char *server_name)
{
	static const unsigned char h2[] = { 2, 'h', '2' };
	OsslSession *s = session_new((OsslSystem *) base);

	if (s == NULL)
		return NULL;
	SSL_set_connect_state(s->ssl);
	SSL_set_alpn_protos(s->ssl, h2, sizeof h2);
	if (server_name != NULL) {
		SSL_set_tlsext_host_name(s->ssl, server_name);
		SSL_set1_host(s->ssl, server_name);	/* hostname check */
	}
	advance(s);		/* produces the ClientHello */
	return &s->base;
}

static void
sys_destroy(ProtobufCGrpc2SecureTransportSystem *base)
{
	OsslSystem *sys = (OsslSystem *) base;
	SSL_CTX_free(sys->ctx);
	free(sys);
}

ProtobufCGrpc2SecureTransportSystem *
protobuf_c_grpc2_secure_transport_system_new_openssl(SSL_CTX *ctx)
{
	OsslSystem *sys = calloc(1, sizeof(*sys));

	if (sys == NULL)
		return NULL;
	SSL_CTX_up_ref(ctx);
	sys->ctx = ctx;
	sys->base.new_server = sys_new_server;
	sys->base.new_client = sys_new_client;
	sys->base.destroy = sys_destroy;
	SSL_CTX_set_alpn_select_cb(ctx, alpn_select, NULL);
	/* gRPC requires TLS 1.2 or later. */
	if (SSL_CTX_get_min_proto_version(ctx) < TLS1_2_VERSION)
		SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
	return &sys->base;
}

int
protobuf_c_grpc2_server_builder_use_openssl(ProtobufCGrpc2ServerBuilder *builder,
					    SSL_CTX *ctx)
{
	ProtobufCGrpc2SecureTransportSystem *sts =
		protobuf_c_grpc2_secure_transport_system_new_openssl(ctx);

	if (sts == NULL)
		return -1;
	if (protobuf_c_grpc2_server_builder_add_secure_transport_system(builder,
									sts) != 0) {
		sts->destroy(sts);
		return -1;
	}
	return 0;
}

int
protobuf_c_grpc2_client_builder_use_openssl(ProtobufCGrpc2ClientBuilder *builder,
					    SSL_CTX *ctx)
{
	ProtobufCGrpc2SecureTransportSystem *sts =
		protobuf_c_grpc2_secure_transport_system_new_openssl(ctx);

	if (sts == NULL)
		return -1;
	if (protobuf_c_grpc2_client_builder_add_secure_transport_system(builder,
									sts) != 0) {
		sts->destroy(sts);
		return -1;
	}
	return 0;
}
