/*
 * Endpoint test: a real libuv server and a real libuv client, both created
 * from options structs (with and without TLS), talk to each other. Everything
 * runs on one libuv loop in this process.
 *
 * Modes: 0 plaintext (h2c), 1 TLS with a trusted certificate,
 *        2 TLS where the client does not trust the server certificate
 *        (every call must fail, and nothing must leak or hang).
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509v3.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
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

/*
 * Option handling: nothing here starts any I/O. On failure the constructors
 * leave the I/O system with the caller.
 */
static void
check_option_handling(uv_loop_t *l, ProtobufCService *const *services)
{
	ProtobufCService *twice[2] = { services[0], services[0] };
	ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
	ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
	ProtobufCGrpc2Server *server;
	ProtobufCGrpc2IoSystem *io = protobuf_c_grpc2_io_system_new_uv(l);

	assert(io != NULL);

	/* a registry for the sans-I/O API: no options, no I/O, cannot listen */
	server = protobuf_c_grpc2_server_new(NULL);
	assert(server != NULL);
	assert(protobuf_c_grpc2_server_add_service(server, services[0]) == 0);
	assert(protobuf_c_grpc2_server_add_service(server, services[0]) == -1);
	assert(protobuf_c_grpc2_server_listen(server, "127.0.0.1", 1) == -1);
	protobuf_c_grpc2_server_free(server);

	/* struct_size is required */
	so.struct_size = 0;
	so.io = io;
	assert(protobuf_c_grpc2_server_new(&so) == NULL);
	so.struct_size = sizeof so;
	co.struct_size = 0;
	assert(protobuf_c_grpc2_client_new(&co) == NULL);
	co.struct_size = sizeof co;

	/* the same service twice is an error, and `io` stays ours */
	so.services = twice;
	so.n_services = 2;
	assert(protobuf_c_grpc2_server_new(&so) == NULL);
	/* TLS without an event loop makes no sense */
	so.services = services;
	so.n_services = 1;
	so.io = NULL;
	so.secure_transport = (ProtobufCGrpc2SecureTransportSystem *) io;
	assert(protobuf_c_grpc2_server_new(&so) == NULL);
	so.secure_transport = NULL;

	/* a client needs a host and an I/O system */
	co.io = io;
	assert(protobuf_c_grpc2_client_new(&co) == NULL);	/* no host */
	co.host = "localhost";
	co.io = NULL;
	assert(protobuf_c_grpc2_client_new(&co) == NULL);	/* no io */

	/* a struct from an older version of the library (fewer fields) works:
	 * the newer fields are read as zero, whatever the memory holds */
	so.io = io;
	so.max_timeout_ms = 12345;		/* beyond the claimed size */
	so.struct_size = offsetof(ProtobufCGrpc2ServerOptions, max_message_size);
	server = protobuf_c_grpc2_server_new(&so);
	assert(server != NULL);			/* it now owns io */
	protobuf_c_grpc2_server_free(server);
	/* let the shutdown complete: it frees the I/O system */
	uv_run(l, UV_RUN_DEFAULT);

	/* io was still ours after the failures above, which is why using it
	 * for the last success was legal */
}

static void
run(int mode)
{
	ProtobufCService *services[] = { NULL };
	ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
	ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
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

	services[0] = &greeter.base;
	check_option_handling(loop, services);

	/* ---- server and client options ---- */
	so.services = services;
	so.n_services = 1;
	so.io = protobuf_c_grpc2_io_system_new_uv(loop);
	assert(so.io != NULL);
	so.on_call_done = NULL;

	co.host = "localhost";
	co.port = port;
	co.io = protobuf_c_grpc2_io_system_new_uv(loop);
	assert(co.io != NULL);
	co.on_call_done = on_call_done;

	if (mode > 0) {
		key = EVP_EC_gen("P-256");
		cert = make_cert(key, "localhost");
		sctx = SSL_CTX_new(TLS_server_method());
		assert(SSL_CTX_use_certificate(sctx, cert) == 1);
		assert(SSL_CTX_use_PrivateKey(sctx, key) == 1);
		so.secure_transport =
			protobuf_c_grpc2_secure_transport_system_new_openssl(sctx);
		assert(so.secure_transport != NULL);
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
		co.secure_transport =
			protobuf_c_grpc2_secure_transport_system_new_openssl(cctx);
		assert(co.secure_transport != NULL);
	}
	server = protobuf_c_grpc2_server_new(&so);
	assert(server != NULL);
	assert(protobuf_c_grpc2_server_listen(server, "127.0.0.1", port) == 0);

	client = protobuf_c_grpc2_client_new(&co);
	assert(client != NULL);
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
	puts("endpoints: plaintext ok");
	run(1);
	puts("endpoints: TLS ok");
	run(2);
	puts("endpoints: untrusted certificate rejected ok");
	puts("endpoints: ok");
	return 0;
}
