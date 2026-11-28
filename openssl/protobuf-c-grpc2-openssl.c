/* OpenSSL implementation of ProtobufCGrpc2SecureTransport. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include "protobuf-c-grpc2-openssl.h"

typedef struct {
	ProtobufCGrpc2SecureTransportSystem base;	/* must be first */
	SSL_CTX *ctx;
	int identity_kind;	/* a ProtobufCGrpc2PeerIdentity */
	int refs;
	int floating;	/* the creation reference is not yet claimed */
} OsslSystem;

typedef struct {
	ProtobufCGrpc2SecureTransport base;		/* must be first */
	SSL *ssl;
	BIO *rbio;		/* ciphertext in (from the network) */
	BIO *wbio;		/* ciphertext out (to the network) */
	int state;		/* 0 handshaking, 1 done, -1 failed/closed */
	char alpn[16];
	int identity_kind;
	int identity_done;	/* `identity` has been worked out */
	char *identity;		/* of the verified peer, or NULL */
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

/* A copy of an ASN.1 string, or NULL if it is empty or has a NUL in it (which
 * would make a name look shorter to C than to the certificate's checker). */
static char *
asn1_to_string(const ASN1_STRING *a)
{
	const unsigned char *d = ASN1_STRING_get0_data(a);
	int n = ASN1_STRING_length(a);
	char *r;

	if (n <= 0 || memchr(d, 0, (size_t) n) != NULL)
		return NULL;
	r = malloc((size_t) n + 1);
	if (r != NULL) {
		memcpy(r, d, (size_t) n);
		r[n] = 0;
	}
	return r;
}

/* The first subject alternative name of the given GEN_* type. */
static char *
san_of_type(X509 *cert, int type)
{
	GENERAL_NAMES *san = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
	char *r = NULL;
	int i;

	if (san == NULL)
		return NULL;
	for (i = 0; i < sk_GENERAL_NAME_num(san) && r == NULL; i++) {
		const GENERAL_NAME *g = sk_GENERAL_NAME_value(san, i);

		if (g->type == type)
			r = asn1_to_string(type == GEN_URI ? g->d.uniformResourceIdentifier
				: type == GEN_DNS ? g->d.dNSName
				: g->d.rfc822Name);
	}
	GENERAL_NAMES_free(san);
	return r;
}

static char *
cert_identity(X509 *cert, int kind)
{
	char *r = NULL;

	if (kind == PROTOBUF_C_GRPC2_PEER_IDENTITY_FINGERPRINT) {
		unsigned char md[EVP_MAX_MD_SIZE];
		unsigned n = 0, i;

		if (X509_digest(cert, EVP_sha256(), md, &n) != 1)
			return NULL;
		r = malloc(2 * (size_t) n + 1);
		for (i = 0; r != NULL && i < n; i++)
			snprintf(r + 2 * i, 3, "%02x", md[i]);
		return r;
	}
	if (kind == PROTOBUF_C_GRPC2_PEER_IDENTITY_SUBJECT) {
		BIO *b = BIO_new(BIO_s_mem());
		char *p;
		long n;

		if (b == NULL)
			return NULL;
		if (X509_NAME_print_ex(b, X509_get_subject_name(cert), 0,
				       XN_FLAG_RFC2253) > 0) {
			n = BIO_get_mem_data(b, &p);
			if (n > 0 && memchr(p, 0, (size_t) n) == NULL &&
			    (r = malloc((size_t) n + 1)) != NULL) {
				memcpy(r, p, (size_t) n);
				r[n] = 0;
			}
		}
		BIO_free(b);
		return r;
	}
	/* AUTO */
	if ((r = san_of_type(cert, GEN_URI)) != NULL ||
	    (r = san_of_type(cert, GEN_DNS)) != NULL ||
	    (r = san_of_type(cert, GEN_EMAIL)) != NULL)
		return r;
	{
		X509_NAME *name = X509_get_subject_name(cert);
		int idx = X509_NAME_get_index_by_NID(name, NID_commonName, -1);

		if (idx >= 0)
			r = asn1_to_string(X509_NAME_ENTRY_get_data(
				X509_NAME_get_entry(name, idx)));
	}
	return r;
}

static const char *
sess_peer_identity(ProtobufCGrpc2SecureTransport *t)
{
	OsslSession *s = (OsslSession *) t;

	if (!s->identity_done && s->state == 1) {
		X509 *cert;

		s->identity_done = 1;
		/* only a certificate that verified says who the peer is */
		if (SSL_get_verify_result(s->ssl) == X509_V_OK) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
			cert = SSL_get1_peer_certificate(s->ssl);
#else
			cert = SSL_get_peer_certificate(s->ssl);
#endif
			if (cert != NULL) {
				s->identity = cert_identity(cert, s->identity_kind);
				X509_free(cert);
			}
		}
	}
	return s->identity;
}

static void
sess_destroy(ProtobufCGrpc2SecureTransport *t)
{
	OsslSession *s = (OsslSession *) t;
	SSL_free(s->ssl);		/* also frees the BIOs */
	free(s->identity);
	free(s);
}

static const ProtobufCGrpc2SecureTransportVTable sess_vtable = {
	sess_feed, sess_read_plaintext, sess_write_plaintext,
	sess_read_ciphertext, sess_handshake_state, sess_alpn, sess_destroy,
	sess_peer_identity
};

static OsslSession *
session_new(OsslSystem *sys)
{
	OsslSession *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return NULL;
	s->base.vtable = &sess_vtable;
	s->identity_kind = sys->identity_kind;
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

/* Like g_object_ref_sink(): the first reference taken claims the creation
 * reference instead of adding one. */
static void
sys_ref(ProtobufCGrpc2SecureTransportSystem *base)
{
	OsslSystem *sys = (OsslSystem *) base;

	if (sys->floating)
		sys->floating = 0;
	else
		sys->refs++;
}

static void
sys_destroy(ProtobufCGrpc2SecureTransportSystem *base)
{
	OsslSystem *sys = (OsslSystem *) base;

	if (--sys->refs > 0)
		return;
	SSL_CTX_free(sys->ctx);
	free(sys);
}

ProtobufCGrpc2SecureTransportSystem *
protobuf_c_grpc2_secure_transport_system_new_openssl(
	const ProtobufCGrpc2OpensslOptions *options)
{
	ProtobufCGrpc2OpensslOptions o;
	OsslSystem *sys;
	SSL_CTX *ctx;

	if (protobuf_c_grpc2_options_copy(&o, sizeof o, options) != 0 ||
	    o.ctx == NULL)
		return NULL;
	ctx = o.ctx;
	if (o.client_auth < PROTOBUF_C_GRPC2_CLIENT_AUTH_NONE ||
	    o.client_auth > PROTOBUF_C_GRPC2_CLIENT_AUTH_REQUIRED ||
	    o.peer_identity < PROTOBUF_C_GRPC2_PEER_IDENTITY_AUTO ||
	    o.peer_identity > PROTOBUF_C_GRPC2_PEER_IDENTITY_FINGERPRINT)
		return NULL;
	sys = calloc(1, sizeof(*sys));
	if (sys == NULL)
		return NULL;
	sys->identity_kind = o.peer_identity;
	SSL_CTX_up_ref(ctx);
	sys->ctx = ctx;
	sys->base.new_server = sys_new_server;
	sys->base.new_client = sys_new_client;
	sys->base.destroy = sys_destroy;
	sys->base.ref = sys_ref;
	sys->refs = 1;
	sys->floating = 1;
	SSL_CTX_set_alpn_select_cb(ctx, alpn_select, NULL);
	if (o.client_auth != PROTOBUF_C_GRPC2_CLIENT_AUTH_NONE)
		SSL_CTX_set_verify(ctx, o.client_auth == PROTOBUF_C_GRPC2_CLIENT_AUTH_REQUIRED
			? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT
			: SSL_VERIFY_PEER, NULL);
	/* gRPC requires TLS 1.2 or later. */
	if (SSL_CTX_get_min_proto_version(ctx) < TLS1_2_VERSION)
		SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
	return &sys->base;
}
