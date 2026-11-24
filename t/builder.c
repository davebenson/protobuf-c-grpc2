/*
 * Builder test: a real libuv server and a real libuv client, both assembled
 * with the builder APIs (with and without TLS), talk to each other. Everything
 * runs on one libuv loop in this process.
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

static Example__HelloReply_Closure slow_closure;
static void *slow_closure_data;

static void
greeter_say_hello_many(Example__Greeter_Service *s,
		       const Example__HelloRequest *req,
		       Example__HelloReply_Closure cl, void *d)
{
	int i;
	(void) s;
	if (strcmp(req->name, "slow") == 0) {
		/* never answers by itself: only the deadline can end this call */
		slow_closure = cl;
		slow_closure_data = d;
		return;
	}
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

/* ---- the run ---- */

static uv_loop_t *loop;
static ProtobufCGrpc2Server *server;
static ProtobufCGrpc2Client *client;
static ProtobufCService *svc;
static uv_timer_t finish_timer;
static int pending;
static int n_ok, n_failed, n_deadline;
static int reply_count;
static char replies[8][160];
static int mode_;
static int probing;			/* a call that must fail synchronously */
static int fast_fail_status;

static void finish_cb(uv_timer_t *t);

static void
on_call_done(const ProtobufCMethodDescriptor *m, int status, const char *msg,
	     void *ud)
{
	(void) m; (void) msg; (void) ud;
	if (probing) {
		fast_fail_status = status;
		return;
	}
	if (status == PROTOBUF_C_GRPC2_OK)
		n_ok++;
	else
		n_failed++;
	if (status == PROTOBUF_C_GRPC2_DEADLINE_EXCEEDED)
		n_deadline++;
	if (--pending == 0)
		uv_timer_start(&finish_timer, finish_cb, 0, 0);
}

static void
on_reply(const Example__HelloReply *r, void *d)
{
	(void) d;
	if (r != NULL && reply_count < 8)
		snprintf(replies[reply_count++], sizeof replies[0], "%.150s", r->message);
}

static void
finish_cb(uv_timer_t *t)
{
	(void) t;
	if (mode_ == 2) {
		/* the failed handshake put the client into backoff: a new call
		 * must fail at once, from inside invoke() */
		Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
		req.name = "again";
		fast_fail_status = -1;
		probing = 1;
		example__greeter__say_hello(svc, &req, on_reply, NULL);
		probing = 0;
		assert(fast_fail_status == PROTOBUF_C_GRPC2_UNAVAILABLE);
	}
	protobuf_c_grpc2_client_free(client);
	client = NULL;
	protobuf_c_grpc2_server_free(server);
	server = NULL;
	uv_close((uv_handle_t *) &finish_timer, NULL);
}

static void
start_calls(void)
{
	ProtobufCService *svc2;
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
	ProtobufCInputStream *in;
	int i;

	pending = 4;
	req.name = "tls";
	example__greeter__say_hello(svc, &req, on_reply, NULL);
	req.count = 2;
	example__greeter__say_hello_many(svc, &req, on_reply, NULL);
	in = example__greeter__collect(svc, NULL, on_reply, NULL);
	assert(in != NULL);
	for (i = 1; i <= 4; i++) {
		req.count = i;
		in->vtable->send_input(in, &req.base);
	}
	in->vtable->send_input(in, NULL);
	in->vtable->destroy(in);

	/* a call that the server never answers, with a 150 ms deadline: the
	 * server's own timer (set up by the builder) must end it */
	svc2 = protobuf_c_grpc2_client_get_service(client,
						   &example__greeter__descriptor);
	assert(protobuf_c_grpc2_client_service_set_timeout_ms(svc2, 150) == 0);
	req.name = "slow";
	req.count = 1;
	example__greeter__say_hello_many(svc2, &req, on_reply, NULL);
}

static void
run(int mode)
{
	ProtobufCGrpc2ConnOptions copts;
	ProtobufCGrpc2ServerBuilder *b;
	ProtobufCGrpc2ClientBuilder *cb;
	Example__Greeter_Service greeter = EXAMPLE__GREETER__INIT(greeter_);
	unsigned port = 20000 + ((unsigned) getpid() * 7 + (unsigned) mode) % 20000;
	EVP_PKEY *key = NULL, *other_key = NULL;
	X509 *cert = NULL, *other_cert = NULL;
	SSL_CTX *sctx = NULL, *cctx = NULL;
	int rv;

	mode_ = mode;
	n_ok = n_failed = n_deadline = reply_count = 0;
	slow_closure = NULL;
	loop = uv_loop_new();
	assert(loop != NULL);

	/* an I/O system that is added but never used is destroyed with the builder */
	b = protobuf_c_grpc2_server_builder_new();
	assert(protobuf_c_grpc2_server_builder_use_uv(b, loop) == 0);
	protobuf_c_grpc2_server_builder_free(b);

	/* ---- server, from the builder ---- */
	b = protobuf_c_grpc2_server_builder_new();
	assert(protobuf_c_grpc2_server_builder_add_service(b, &greeter.base) == 0);
	assert(protobuf_c_grpc2_server_builder_add_service(b, &greeter.base) == -1);
	assert(protobuf_c_grpc2_server_from_builder(b) == NULL);	/* no I/O system */
	assert(protobuf_c_grpc2_server_builder_use_uv(b, loop) == 0);
	assert(protobuf_c_grpc2_server_builder_use_uv(b, loop) != 0);	/* only one */

	/* ---- client, from the builder ---- */
	cb = protobuf_c_grpc2_client_builder_new();
	assert(protobuf_c_grpc2_client_from_builder(cb) == NULL);	/* no target */
	assert(protobuf_c_grpc2_client_builder_set_target(cb, "localhost", port) == 0);
	assert(protobuf_c_grpc2_client_from_builder(cb) == NULL);	/* no I/O */
	assert(protobuf_c_grpc2_client_builder_use_uv(cb, loop) == 0);
	assert(protobuf_c_grpc2_client_builder_use_uv(cb, loop) != 0);
	memset(&copts, 0, sizeof copts);
	copts.on_call_done = on_call_done;
	assert(protobuf_c_grpc2_client_builder_set_options(cb, &copts) == 0);

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
		assert(protobuf_c_grpc2_client_builder_use_openssl(cb, cctx) == 0);
	}
	server = protobuf_c_grpc2_server_from_builder(b);
	assert(server != NULL);
	protobuf_c_grpc2_server_builder_free(b);
	assert(protobuf_c_grpc2_server_listen(server, "127.0.0.1", port) == 0);

	client = protobuf_c_grpc2_client_from_builder(cb);
	assert(client != NULL);
	protobuf_c_grpc2_client_builder_free(cb);
	svc = protobuf_c_grpc2_client_get_service(client,
						  &example__greeter__descriptor);
	assert(svc != NULL);

	uv_timer_init(loop, &finish_timer);
	start_calls();
	uv_run(loop, UV_RUN_DEFAULT);

	/* ---- results ---- */
	if (mode == 2) {
		assert(n_ok == 0 && n_failed == 4 && reply_count == 0);
	} else {
		int i, found_hello = 0, found_total = 0, found_2 = 0;
		assert(n_ok == 3 && n_failed == 1 && n_deadline == 1);
		assert(slow_closure != NULL);
		assert(reply_count == 4);
		for (i = 0; i < reply_count; i++) {
			found_hello |= strcmp(replies[i], "Hello, tls!") == 0;
			found_2 |= strcmp(replies[i], "#2") == 0;
			found_total |= strcmp(replies[i], "total=10") == 0;
		}
		assert(found_hello && found_2 && found_total);
	}
	assert(server == NULL && client == NULL);
	if (slow_closure != NULL)
		slow_closure(NULL, slow_closure_data);	/* ends it; frees the call */
	rv = uv_loop_close(loop);		/* fails if any handle leaked */
	assert(rv == 0);
	free(loop);

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
