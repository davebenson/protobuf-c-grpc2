/*
 * Builder test: a real libuv server assembled with the builder API (with
 * and without TLS) is exercised by a client that uses the library's
 * sans-I/O client plus the OpenSSL client transport. Everything runs on one
 * libuv loop in this process.
 *
 * Modes: 0 plaintext (h2c), 1 TLS with a trusted certificate,
 *        2 TLS where the client does not trust the server certificate
 *        (every call must fail, and nothing must leak or hang).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509v3.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2-builder.h"
#include "protobuf-c-grpc2/protobuf-c-grpc2-openssl.h"
#include "protobuf-c-grpc2/protobuf-c-grpc2-uv.h"
#include "greeter.pb-c.h"

/* ---- the service ---- */

static void
greeter_say_hello(Example__Greeter_Service *s, const Example__HelloRequest *req,
		  Example__HelloReply_Closure cl, void *d)
{
	Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
	size_t n = strlen(req->name) + 16;
	char *text = malloc(n);
	(void) s;
	snprintf(text, n, "Hello, %s!", req->name);
	reply.message = text;
	cl(&reply, d);
	free(text);
}

static void
greeter_say_hello_many(Example__Greeter_Service *s,
		       const Example__HelloRequest *req,
		       Example__HelloReply_Closure cl, void *d)
{
	int i;
	(void) s;
	for (i = 1; i <= req->count; i++) {
		Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
		char text[64];
		snprintf(text, sizeof text, "#%d", i);
		reply.message = text;
		cl(&reply, d);
	}
	cl(NULL, d);
}

typedef struct {
	ProtobufCInputStream base;
	int total;
	Example__HelloReply_Closure cl;
	void *d;
} SumStream;

static protobuf_c_boolean
sum_send(ProtobufCInputStream *st, const ProtobufCMessage *m)
{
	SumStream *ss = (SumStream *) st;
	if (m != NULL) {
		ss->total += ((const Example__HelloRequest *) m)->count;
	} else {
		Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
		char text[64];
		snprintf(text, sizeof text, "total=%d", ss->total);
		reply.message = text;
		ss->cl(&reply, ss->d);
	}
	return 1;
}

static void
sum_destroy(ProtobufCInputStream *st)
{
	free(st);
}

static const ProtobufCInputStreamVTable sum_vtable = { sum_send, sum_destroy };

static ProtobufCInputStream *
greeter_collect(Example__Greeter_Service *s, const Example__HelloRequest *first,
		Example__HelloReply_Closure cl, void *d)
{
	SumStream *ss = calloc(1, sizeof(*ss));
	(void) s;
	ss->base.vtable = &sum_vtable;
	ss->cl = cl;
	ss->d = d;
	if (first != NULL)
		sum_send(&ss->base, &first->base);
	return &ss->base;
}

/* ---- certificates ---- */

static X509 *
make_cert(EVP_PKEY *key, const char *cn)
{
	X509 *x = X509_new();
	X509_NAME *name;
	X509V3_CTX ctx;
	X509_EXTENSION *ext;

	X509_set_version(x, 2);
	ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
	X509_gmtime_adj(X509_getm_notBefore(x), -60);
	X509_gmtime_adj(X509_getm_notAfter(x), 3600);
	X509_set_pubkey(x, key);
	name = X509_get_subject_name(x);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
				   (const unsigned char *) cn, -1, -1, 0);
	X509_set_issuer_name(x, name);
	X509V3_set_ctx_nodb(&ctx);
	X509V3_set_ctx(&ctx, x, x, NULL, NULL, 0);
	ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_subject_alt_name,
				  "DNS:localhost");
	assert(ext != NULL);
	X509_add_ext(x, ext, -1);
	X509_EXTENSION_free(ext);
	assert(X509_sign(x, key, EVP_sha256()) > 0);
	return x;
}

/* ---- client harness ---- */

typedef struct {
	uv_tcp_t tcp;
	uv_check_t check;
	ProtobufCGrpc2Conn *conn;
	ProtobufCGrpc2SecureTransport *tls;
	int closing, handles;
	char rbuf[16384];
} TestClient;

static uv_loop_t *loop;
static ProtobufCGrpc2Server *server;
static TestClient *client;
static int pending;
static int n_ok, n_failed;
static int reply_count;
static char replies[8][160];

static void client_close(TestClient *c);

static void
client_handle_closed(uv_handle_t *h)
{
	TestClient *c = h->data;
	if (--c->handles > 0)
		return;
	protobuf_c_grpc2_conn_free(c->conn);	/* completes unfinished calls */
	if (c->tls != NULL)
		c->tls->vtable->destroy(c->tls);
	free(c);
	client = NULL;
	protobuf_c_grpc2_server_free(server);	/* closes server connections */
	server = NULL;
}

static void
client_close(TestClient *c)
{
	if (c->closing)
		return;
	c->closing = 1;
	c->handles = 2;
	uv_read_stop((uv_stream_t *) &c->tcp);
	uv_close((uv_handle_t *) &c->tcp, client_handle_closed);
	uv_close((uv_handle_t *) &c->check, client_handle_closed);
}

static void
write_done(uv_write_t *req, int status)
{
	(void) status;
	free(req);
}

static void
client_send(TestClient *c, const uint8_t *buf, size_t n)
{
	uv_write_t *req = malloc(sizeof(*req) + n);
	uv_buf_t b;
	memcpy(req + 1, buf, n);
	b = uv_buf_init((char *) (req + 1), (unsigned) n);
	if (uv_write(req, (uv_stream_t *) &c->tcp, &b, 1, write_done) != 0)
		free(req);
}

static int
client_ready(TestClient *c)
{
	int st;
	if (c->tls == NULL)
		return 1;
	st = c->tls->vtable->handshake_state(c->tls);
	if (st < 0) {
		client_close(c);
		return 0;
	}
	if (st == 1) {
		const char *alpn = c->tls->vtable->alpn(c->tls);
		assert(alpn != NULL && strcmp(alpn, "h2") == 0);
	}
	return st == 1;
}

static void
client_flush(uv_check_t *chk)
{
	TestClient *c = chk->data;
	uint8_t buf[16384];
	ssize_t n;

	if (c->closing)
		return;
	if (client_ready(c)) {
		while ((n = protobuf_c_grpc2_conn_write(c->conn, buf, sizeof buf)) > 0) {
			if (c->tls == NULL)
				client_send(c, buf, (size_t) n);
			else
				c->tls->vtable->write_plaintext(c->tls, buf, (size_t) n);
		}
	}
	if (c->tls != NULL && !c->closing)
		while ((n = c->tls->vtable->read_ciphertext(c->tls, buf, sizeof buf)) > 0)
			client_send(c, buf, (size_t) n);
}

static void
client_alloc(uv_handle_t *h, size_t sz, uv_buf_t *buf)
{
	TestClient *c = h->data;
	(void) sz;
	*buf = uv_buf_init(c->rbuf, sizeof c->rbuf);
}

static void
client_read(uv_stream_t *s, ssize_t nread, const uv_buf_t *buf)
{
	TestClient *c = s->data;
	uint8_t plain[16384];

	if (nread < 0) {
		client_close(c);
		return;
	}
	if (c->tls == NULL) {
		if (protobuf_c_grpc2_conn_read(c->conn, (const uint8_t *) buf->base,
					       (size_t) nread) < 0)
			client_close(c);
		return;
	}
	if (c->tls->vtable->feed_ciphertext(c->tls, (const uint8_t *) buf->base,
					    (size_t) nread) < 0 ||
	    !client_ready(c))
		return;
	for (;;) {
		ssize_t n = c->tls->vtable->read_plaintext(c->tls, plain, sizeof plain);
		if (n <= 0)
			break;
		if (protobuf_c_grpc2_conn_read(c->conn, plain, (size_t) n) < 0) {
			client_close(c);
			return;
		}
	}
}

static void
on_call_done(const ProtobufCMethodDescriptor *m, int status, const char *msg,
	     void *ud)
{
	(void) m; (void) msg; (void) ud;
	if (status == PROTOBUF_C_GRPC2_OK)
		n_ok++;
	else
		n_failed++;
	if (--pending == 0 && client != NULL)
		client_close(client);
}

static void
on_reply(const Example__HelloReply *r, void *d)
{
	(void) d;
	if (r != NULL && reply_count < 8)
		snprintf(replies[reply_count++], sizeof replies[0], "%.150s", r->message);
}

static void
start_calls(TestClient *c)
{
	ProtobufCService *svc = protobuf_c_grpc2_conn_client_service(
		c->conn, &example__greeter__descriptor);
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
	ProtobufCInputStream *in;
	int i;

	pending = 3;
	req.name = "tls";
	example__greeter__say_hello(svc, &req, on_reply, NULL);
	req.count = 2;
	example__greeter__say_hello_many(svc, &req, on_reply, NULL);
	in = example__greeter__collect(svc, NULL, on_reply, NULL);
	for (i = 1; i <= 4; i++) {
		req.count = i;
		in->vtable->send_input(in, &req.base);
	}
	in->vtable->send_input(in, NULL);
	in->vtable->destroy(in);
}

static void
on_connect(uv_connect_t *req, int status)
{
	TestClient *c = req->data;
	free(req);
	assert(status == 0);
	uv_read_start((uv_stream_t *) &c->tcp, client_alloc, client_read);
	start_calls(c);
}

static void
run(int mode)
{
	ProtobufCGrpc2ConnOptions copts;
	ProtobufCGrpc2ServerBuilder *b;
	Example__Greeter_Service greeter = EXAMPLE__GREETER__INIT(greeter_);
	unsigned port = 20000 + ((unsigned) getpid() * 7 + (unsigned) mode) % 20000;
	EVP_PKEY *key = NULL, *other_key = NULL;
	X509 *cert = NULL, *other_cert = NULL;
	SSL_CTX *sctx = NULL, *cctx = NULL;
	ProtobufCGrpc2SecureTransportSystem *csts = NULL;
	uv_connect_t *creq;
	struct sockaddr_in addr;
	int rv;

	n_ok = n_failed = reply_count = 0;
	loop = uv_loop_new();
	assert(loop != NULL);

	/* ---- server, from the builder ---- */
	/* an I/O system that is added but never used is destroyed with the builder */
	b = protobuf_c_grpc2_server_builder_new();
	assert(protobuf_c_grpc2_server_builder_use_uv(b, loop) == 0);
	protobuf_c_grpc2_server_builder_free(b);

	b = protobuf_c_grpc2_server_builder_new();
	assert(protobuf_c_grpc2_server_builder_add_service(b, &greeter.base) == 0);
	assert(protobuf_c_grpc2_server_builder_add_service(b, &greeter.base) == -1);
	assert(protobuf_c_grpc2_server_from_builder(b) == NULL);	/* no I/O system */
	assert(protobuf_c_grpc2_server_builder_use_uv(b, loop) == 0);
	assert(protobuf_c_grpc2_server_builder_use_uv(b, loop) != 0);	/* only one */

	if (mode > 0) {
		key = EVP_EC_gen("P-256");
		cert = make_cert(key, "localhost");
		sctx = SSL_CTX_new(TLS_server_method());
		assert(SSL_CTX_use_certificate(sctx, cert) == 1);
		assert(SSL_CTX_use_PrivateKey(sctx, key) == 1);
		assert(protobuf_c_grpc2_server_builder_use_openssl(b, sctx) == 0);
		assert(SSL_CTX_get_min_proto_version(sctx) >= TLS1_2_VERSION);

		cctx = SSL_CTX_new(TLS_client_method());
		SSL_CTX_set_verify(cctx, SSL_VERIFY_PEER, NULL);
		if (mode == 1) {
			X509_STORE_add_cert(SSL_CTX_get_cert_store(cctx), cert);
		} else {
			other_key = EVP_EC_gen("P-256");
			other_cert = make_cert(other_key, "localhost");
			X509_STORE_add_cert(SSL_CTX_get_cert_store(cctx), other_cert);
		}
		csts = protobuf_c_grpc2_secure_transport_system_new_openssl(cctx);
		assert(csts != NULL);
	}
	server = protobuf_c_grpc2_server_from_builder(b);
	assert(server != NULL);
	protobuf_c_grpc2_server_builder_free(b);
	assert(protobuf_c_grpc2_server_listen(server, "127.0.0.1", port) == 0);

	/* ---- client ---- */
	memset(&copts, 0, sizeof copts);
	copts.on_call_done = on_call_done;
	copts.authority = "localhost";
	copts.scheme = mode > 0 ? "https" : "http";
	client = calloc(1, sizeof(*client));
	client->conn = protobuf_c_grpc2_conn_client_new(&copts);
	if (csts != NULL) {
		client->tls = csts->new_client(csts, "localhost");
		assert(client->tls != NULL);
	}
	uv_tcp_init(loop, &client->tcp);
	uv_check_init(loop, &client->check);
	client->tcp.data = client;
	client->check.data = client;
	uv_check_start(&client->check, client_flush);
	uv_unref((uv_handle_t *) &client->check);
	creq = malloc(sizeof(*creq));
	creq->data = client;
	uv_ip4_addr("127.0.0.1", (int) port, &addr);
	rv = uv_tcp_connect(creq, &client->tcp, (const struct sockaddr *) &addr,
			    on_connect);
	assert(rv == 0);

	uv_run(loop, UV_RUN_DEFAULT);

	/* ---- results ---- */
	if (mode == 2) {
		assert(n_ok == 0 && n_failed == 3 && reply_count == 0);
	} else {
		int i, found_hello = 0, found_total = 0, found_2 = 0;
		assert(n_ok == 3 && n_failed == 0);
		assert(reply_count == 4);
		for (i = 0; i < reply_count; i++) {
			found_hello |= strcmp(replies[i], "Hello, tls!") == 0;
			found_2 |= strcmp(replies[i], "#2") == 0;
			found_total |= strcmp(replies[i], "total=10") == 0;
		}
		assert(found_hello && found_2 && found_total);
	}
	assert(server == NULL && client == NULL);
	rv = uv_loop_close(loop);		/* fails if any handle leaked */
	assert(rv == 0);
	free(loop);

	if (csts != NULL)
		csts->destroy(csts);
	SSL_CTX_free(cctx);
	SSL_CTX_free(sctx);
	X509_free(cert);
	X509_free(other_cert);
	EVP_PKEY_free(key);
	EVP_PKEY_free(other_key);
}

int
main(void)
{
	run(0);
	puts("builder: plaintext ok");
	run(1);
	puts("builder: TLS ok");
	run(2);
	puts("builder: untrusted certificate rejected ok");
	puts("builder: ok");
	return 0;
}
