/*
 * GnuTLS implementation of ProtobufCGrpc2SecureTransport.
 *
 * A session is driven entirely through memory: GnuTLS is given a push and a
 * pull function that append to and take from two buffers, so the I/O system
 * does the networking. The pull function reports "would block" when the
 * input buffer is empty, which is how GnuTLS learns that it must wait for
 * more ciphertext.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include <gnutls/x509.h>

#include "protobuf-c-grpc2-gnutls.h"

#define DEFAULT_PRIORITY "NORMAL:-VERS-ALL:+VERS-TLS1.3:+VERS-TLS1.2"

typedef struct {
	ProtobufCGrpc2SecureTransportSystem base;	/* must be first */
	gnutls_certificate_credentials_t cred;
	gnutls_priority_t prio;
	int client_auth;	/* a ProtobufCGrpc2ClientAuth, for servers */
	int identity_kind;	/* a ProtobufCGrpc2PeerIdentity */
	int refs;
	int floating;	/* the creation reference is not yet claimed */
} GtlsSystem;

/* A byte queue: append at the end, take from the front. */
typedef struct {
	uint8_t *p;
	size_t len, off, cap;
} Buf;

typedef struct {
	ProtobufCGrpc2SecureTransport base;		/* must be first */
	gnutls_session_t session;
	Buf in;			/* ciphertext from the network */
	Buf out;		/* ciphertext for the network */
	int state;		/* 0 handshaking, 1 done, -1 failed/closed */
	char alpn[16];
	int is_server;
	int client_auth;
	int identity_kind;
	int verified;		/* the peer's certificate chain verified */
	int identity_done;	/* `identity` has been worked out */
	char *identity;
} GtlsSession;

static int
buf_append(Buf *b, const void *data, size_t len)
{
	if (b->off == b->len)
		b->off = b->len = 0;
	if (b->len + len > b->cap) {
		size_t cap = b->cap ? b->cap : 4096;
		uint8_t *np;

		if (b->off > 0) {	/* reclaim the consumed prefix first */
			memmove(b->p, b->p + b->off, b->len - b->off);
			b->len -= b->off;
			b->off = 0;
		}
		while (cap < b->len + len)
			cap *= 2;
		np = realloc(b->p, cap);
		if (np == NULL)
			return -1;
		b->p = np;
		b->cap = cap;
	}
	memcpy(b->p + b->len, data, len);
	b->len += len;
	return 0;
}

static size_t
buf_take(Buf *b, void *dst, size_t len)
{
	size_t n = b->len - b->off;

	if (n > len)
		n = len;
	if (n == 0)
		return 0;	/* (the buffer may not even exist yet) */
	memcpy(dst, b->p + b->off, n);
	b->off += n;
	return n;
}

static ssize_t
gt_push(gnutls_transport_ptr_t ptr, const void *data, size_t len)
{
	GtlsSession *s = ptr;

	if (buf_append(&s->out, data, len) != 0) {
		gnutls_transport_set_errno(s->session, ENOMEM);
		return -1;
	}
	return (ssize_t) len;
}

static ssize_t
gt_pull(gnutls_transport_ptr_t ptr, void *data, size_t len)
{
	GtlsSession *s = ptr;

	if (s->in.off == s->in.len) {
		gnutls_transport_set_errno(s->session, EAGAIN);
		return -1;
	}
	return (ssize_t) buf_take(&s->in, data, len);
}

/* The session has failed with GnuTLS error `err`: tell the peer why where
 * the protocol has an alert for it (GnuTLS does not always do that by
 * itself), so that it does not wait for the rest of a handshake that will
 * never come. The alert is left in the output buffer. */
static void
fail(GtlsSession *s, int err)
{
	if (s->state >= 0 && err < 0)
		gnutls_alert_send_appropriate(s->session, err);
	s->state = -1;
}

/* Run the handshake as far as the data available allows. */
static void
advance(GtlsSession *s)
{
	int rv;

	if (s->state != 0)
		return;
	do {
		rv = gnutls_handshake(s->session);
		/* interruptions and warning alerts are not errors: go on */
	} while (rv < 0 && rv != GNUTLS_E_AGAIN && !gnutls_error_is_fatal(rv));
	if (rv == GNUTLS_E_SUCCESS) {
		s->state = 1;
		/* a client's handshake fails unless the server's certificate
		 * verified (see sys_new_client) */
		if (!s->is_server)
			s->verified = 1;
	}
	else if (rv != GNUTLS_E_AGAIN)
		fail(s, rv);
}

static int
sess_feed(ProtobufCGrpc2SecureTransport *t, const uint8_t *data, size_t len)
{
	GtlsSession *s = (GtlsSession *) t;

	if (s->state < 0)
		return -1;
	if (len > 0 && buf_append(&s->in, data, len) != 0) {
		s->state = -1;
		return -1;
	}
	advance(s);
	return s->state < 0 ? -1 : 0;
}

static ssize_t
sess_read_plaintext(ProtobufCGrpc2SecureTransport *t, uint8_t *buf, size_t len)
{
	GtlsSession *s = (GtlsSession *) t;

	if (s->state < 0)
		return -1;
	if (s->state == 0)
		return 0;
	for (;;) {
		ssize_t n = gnutls_record_recv(s->session, buf, len);

		if (n > 0)
			return n;
		if (n == 0) {		/* the peer closed the session */
			s->state = -1;
			return -1;
		}
		if (n == GNUTLS_E_AGAIN)
			return 0;
		if (n == GNUTLS_E_INTERRUPTED ||
		    n == GNUTLS_E_WARNING_ALERT_RECEIVED)
			continue;
		if (n == GNUTLS_E_REHANDSHAKE) {
			/* gRPC over HTTP/2 does not allow renegotiation */
			gnutls_alert_send(s->session, GNUTLS_AL_WARNING,
					  GNUTLS_A_NO_RENEGOTIATION);
			continue;
		}
		fail(s, (int) n);
		return -1;
	}
}

static ssize_t
sess_write_plaintext(ProtobufCGrpc2SecureTransport *t, const uint8_t *data,
		     size_t len)
{
	GtlsSession *s = (GtlsSession *) t;
	size_t off = 0;

	if (s->state != 1)
		return -1;
	while (off < len) {
		ssize_t n = gnutls_record_send(s->session, data + off,
					       len - off);

		if (n > 0) {
			off += (size_t) n;
		} else if (n == GNUTLS_E_AGAIN || n == GNUTLS_E_INTERRUPTED) {
			continue;	/* the push function never blocks */
		} else {
			s->state = -1;
			return -1;
		}
	}
	return (ssize_t) len;
}

static ssize_t
sess_read_ciphertext(ProtobufCGrpc2SecureTransport *t, uint8_t *buf, size_t len)
{
	GtlsSession *s = (GtlsSession *) t;

	return (ssize_t) buf_take(&s->out, buf, len);
}

static int
sess_handshake_state(ProtobufCGrpc2SecureTransport *t)
{
	return ((GtlsSession *) t)->state;
}

static const char *
sess_alpn(ProtobufCGrpc2SecureTransport *t)
{
	GtlsSession *s = (GtlsSession *) t;
	gnutls_datum_t d;

	if (gnutls_alpn_get_selected_protocol(s->session, &d) != 0 ||
	    d.data == NULL || d.size == 0 || d.size >= sizeof s->alpn)
		return NULL;
	memcpy(s->alpn, d.data, d.size);
	s->alpn[d.size] = 0;
	return s->alpn;
}

/* A copy of `n` bytes as a string, or NULL if empty or with a NUL in it. */
static char *
bytes_to_string(const void *d, size_t n)
{
	char *r;

	if (n == 0 || memchr(d, 0, n) != NULL)
		return NULL;
	r = malloc(n + 1);
	if (r != NULL) {
		memcpy(r, d, n);
		r[n] = 0;
	}
	return r;
}

/* The first subject alternative name of the given type. */
static char *
san_of_type(gnutls_x509_crt_t crt, int type)
{
	unsigned i;

	for (i = 0; ; i++) {
		char buf[512];
		size_t size = sizeof buf;
		int t = gnutls_x509_crt_get_subject_alt_name(crt, i, buf, &size, NULL);

		if (t == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE)
			return NULL;	/* no more */
		if (t < 0)
			continue;	/* too long, say: skip it */
		if (t == type)
			return bytes_to_string(buf, size);
	}
}

static char *
cert_identity(gnutls_x509_crt_t crt, int kind)
{
	char *r = NULL;

	if (kind == PROTOBUF_C_GRPC2_PEER_IDENTITY_FINGERPRINT) {
		unsigned char md[32];
		size_t n = sizeof md, i;

		if (gnutls_x509_crt_get_fingerprint(crt, GNUTLS_DIG_SHA256, md, &n) != 0)
			return NULL;
		r = malloc(2 * n + 1);
		for (i = 0; r != NULL && i < n; i++)
			snprintf(r + 2 * i, 3, "%02x", md[i]);
		return r;
	}
	if (kind == PROTOBUF_C_GRPC2_PEER_IDENTITY_SUBJECT) {
		gnutls_datum_t dn;

		if (gnutls_x509_crt_get_dn3(crt, &dn, 0) != 0)
			return NULL;
		r = bytes_to_string(dn.data, dn.size);
		gnutls_free(dn.data);
		return r;
	}
	if ((r = san_of_type(crt, GNUTLS_SAN_URI)) != NULL ||
	    (r = san_of_type(crt, GNUTLS_SAN_DNSNAME)) != NULL ||
	    (r = san_of_type(crt, GNUTLS_SAN_RFC822NAME)) != NULL)
		return r;
	{
		char buf[512];
		size_t size = sizeof buf;

		if (gnutls_x509_crt_get_dn_by_oid(crt, GNUTLS_OID_X520_COMMON_NAME,
						  0, 0, buf, &size) == 0)
			r = bytes_to_string(buf, size);
	}
	return r;
}

static const char *
sess_peer_identity(ProtobufCGrpc2SecureTransport *t)
{
	GtlsSession *s = (GtlsSession *) t;

	if (!s->identity_done && s->state == 1) {
		unsigned n = 0;
		const gnutls_datum_t *certs;

		s->identity_done = 1;
		/* only a certificate that verified says who the peer is */
		certs = s->verified ? gnutls_certificate_get_peers(s->session, &n)
				    : NULL;
		if (certs != NULL && n > 0) {
			gnutls_x509_crt_t crt;

			if (gnutls_x509_crt_init(&crt) == 0) {
				if (gnutls_x509_crt_import(crt, &certs[0],
							   GNUTLS_X509_FMT_DER) == 0)
					s->identity = cert_identity(crt,
								    s->identity_kind);
				gnutls_x509_crt_deinit(crt);
			}
		}
	}
	return s->identity;
}

static void
sess_destroy(ProtobufCGrpc2SecureTransport *t)
{
	GtlsSession *s = (GtlsSession *) t;

	free(s->identity);
	gnutls_deinit(s->session);
	free(s->in.p);
	free(s->out.p);
	free(s);
}

static const ProtobufCGrpc2SecureTransportVTable sess_vtable = {
	sess_feed, sess_read_plaintext, sess_write_plaintext,
	sess_read_ciphertext, sess_handshake_state, sess_alpn, sess_destroy,
	sess_peer_identity
};

static GtlsSession *
session_new(GtlsSystem *sys, int server)
{
	static const gnutls_datum_t h2 = { (unsigned char *) "h2", 2 };
	GtlsSession *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return NULL;
	s->base.vtable = &sess_vtable;
	s->is_server = server;
	s->identity_kind = sys->identity_kind;
	if (gnutls_init(&s->session, server ? GNUTLS_SERVER : GNUTLS_CLIENT) !=
	    GNUTLS_E_SUCCESS) {
		free(s);
		return NULL;
	}
	if (gnutls_priority_set(s->session, sys->prio) != GNUTLS_E_SUCCESS ||
	    gnutls_credentials_set(s->session, GNUTLS_CRD_CERTIFICATE,
				   sys->cred) != GNUTLS_E_SUCCESS ||
	    /* the server insists on h2; the client only offers it and the
	     * core checks what was chosen */
	    gnutls_alpn_set_protocols(s->session, &h2, 1,
				      server ? GNUTLS_ALPN_MANDATORY : 0) !=
	    GNUTLS_E_SUCCESS) {
		gnutls_deinit(s->session);
		free(s);
		return NULL;
	}
	gnutls_session_set_ptr(s->session, s);
	gnutls_transport_set_ptr(s->session, s);
	gnutls_transport_set_push_function(s->session, gt_push);
	gnutls_transport_set_pull_function(s->session, gt_pull);
	return s;
}

/* Mutual TLS, server side: runs during the handshake once the client's
 * certificate has arrived, and fails it if the chain does not verify against
 * the trust roots of the credentials. */
static int
verify_client(gnutls_session_t session)
{
	GtlsSession *s = gnutls_session_get_ptr(session);
	unsigned status;
	int rv = gnutls_certificate_verify_peers3(session, NULL, &status);

	if (rv == GNUTLS_E_NO_CERTIFICATE_FOUND)
		return s->client_auth == PROTOBUF_C_GRPC2_CLIENT_AUTH_REQUIRED ? -1 : 0;
	if (rv < 0 || status != 0)
		return -1;
	s->verified = 1;
	return 0;
}

static ProtobufCGrpc2SecureTransport *
sys_new_server(ProtobufCGrpc2SecureTransportSystem *base)
{
	GtlsSystem *sys = (GtlsSystem *) base;
	GtlsSession *s = session_new(sys, 1);

	if (s == NULL)
		return NULL;
	if (sys->client_auth != PROTOBUF_C_GRPC2_CLIENT_AUTH_NONE) {
		s->client_auth = sys->client_auth;
		gnutls_certificate_server_set_request(s->session,
			sys->client_auth == PROTOBUF_C_GRPC2_CLIENT_AUTH_REQUIRED
				? GNUTLS_CERT_REQUIRE : GNUTLS_CERT_REQUEST);
		gnutls_session_set_verify_function(s->session, verify_client);
	}
	return &s->base;
}

static int
is_ip_literal(const char *name)
{
	/* a name made only of hex digits, '.' and ':' (SNI must not be an IP) */
	return strchr(name, ':') != NULL ||
	       strspn(name, "0123456789.") == strlen(name);
}

static ProtobufCGrpc2SecureTransport *
sys_new_client(ProtobufCGrpc2SecureTransportSystem *base,
	       const char *server_name)
{
	GtlsSession *s = session_new((GtlsSystem *) base, 0);

	if (s == NULL)
		return NULL;
	if (server_name != NULL && !is_ip_literal(server_name) &&
	    gnutls_server_name_set(s->session, GNUTLS_NAME_DNS, server_name,
				   strlen(server_name)) != GNUTLS_E_SUCCESS)
		goto fail;
	/* the certificate chain and, with a name, the name are checked during
	 * the handshake, which fails if they do not verify */
	gnutls_session_set_verify_cert(s->session, server_name, 0);
	advance(s);		/* produces the ClientHello */
	return &s->base;
fail:
	sess_destroy(&s->base);
	return NULL;
}

/* Like g_object_ref_sink(): the first reference taken claims the creation
 * reference instead of adding one. */
static void
sys_ref(ProtobufCGrpc2SecureTransportSystem *base)
{
	GtlsSystem *sys = (GtlsSystem *) base;

	if (sys->floating)
		sys->floating = 0;
	else
		sys->refs++;
}

static void
sys_free(GtlsSystem *sys)
{
	if (sys->prio != NULL)
		gnutls_priority_deinit(sys->prio);
	gnutls_certificate_free_credentials(sys->cred);
	free(sys);
}

static void
sys_destroy(ProtobufCGrpc2SecureTransportSystem *base)
{
	GtlsSystem *sys = (GtlsSystem *) base;

	if (--sys->refs > 0)
		return;
	sys_free(sys);
}

ProtobufCGrpc2SecureTransportSystem *
protobuf_c_grpc2_secure_transport_system_new_gnutls(
	const ProtobufCGrpc2GnutlsOptions *options)
{
	ProtobufCGrpc2GnutlsOptions o;
	gnutls_certificate_credentials_t cred;
	GtlsSystem *sys;

	if (options == NULL || options->struct_size <
	    offsetof(ProtobufCGrpc2GnutlsOptions, credentials) +
	    sizeof options->credentials)
		return NULL;
	/* read only as much as the caller's struct has: the rest is zero */
	memset(&o, 0, sizeof o);
	memcpy(&o, options, options->struct_size < sizeof o ?
			    options->struct_size : sizeof o);
	cred = o.credentials;
	if (cred == NULL)
		return NULL;
	if (o.client_auth < PROTOBUF_C_GRPC2_CLIENT_AUTH_NONE ||
	    o.client_auth > PROTOBUF_C_GRPC2_CLIENT_AUTH_REQUIRED ||
	    o.peer_identity < PROTOBUF_C_GRPC2_PEER_IDENTITY_AUTO ||
	    o.peer_identity > PROTOBUF_C_GRPC2_PEER_IDENTITY_FINGERPRINT) {
		gnutls_certificate_free_credentials(cred);
		return NULL;
	}
	sys = calloc(1, sizeof(*sys));
	if (sys == NULL) {
		gnutls_certificate_free_credentials(cred);
		return NULL;
	}
	sys->cred = cred;		/* ours from here on, whatever happens */
	sys->client_auth = o.client_auth;
	sys->identity_kind = o.peer_identity;
	if (gnutls_priority_init(&sys->prio,
				 o.policy != NULL ? o.policy : DEFAULT_PRIORITY,
				 NULL) != GNUTLS_E_SUCCESS) {
		sys->prio = NULL;
		sys_free(sys);
		return NULL;
	}
	sys->base.new_server = sys_new_server;
	sys->base.new_client = sys_new_client;
	sys->base.destroy = sys_destroy;
	sys->base.ref = sys_ref;
	sys->refs = 1;
	sys->floating = 1;
	return &sys->base;
}
