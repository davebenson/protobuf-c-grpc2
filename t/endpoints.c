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

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
#include "greeter.pb-c.h"
#include "loop-backend.h"
#include "tls-backend.h"
#include "free-port.h"

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
		free(ss);		/* the end of the stream releases it */
	}
	return 1;
}

static ProtobufCInputStream *
greeter_collect(Example__Greeter_Service *s, const Example__HelloRequest *first,
		Example__HelloReply_Closure cl, void *d)
{
	SumStream *ss = calloc(1, sizeof(*ss));
	(void) s;
	ss->base.send = sum_send;
	ss->base.input_descriptor = &example__hello_request__descriptor;
	ss->cl = cl;
	ss->d = d;
	if (first != NULL)
		sum_send(&ss->base, &first->base);
	return &ss->base;
}

/* ---- the run ---- */

static TestLoop *loop;
static ProtobufCGrpc2Server *server;
static ProtobufCGrpc2Client *client;
static ProtobufCService *svc;
static ProtobufCGrpc2IoSystem *timer_io;	/* only for finish_timer */
static void *finish_timer;
static int pending;
static int n_ok, n_failed, n_deadline;
static int reply_count;
static char replies[8][160];
static int mode_;
static int probing;			/* a call that must fail synchronously */
static int fast_fail_status;

static void finish_cb(void *unused);

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
		timer_io->timer_start(timer_io, finish_timer, 0);
}

static void
on_reply(const Example__HelloReply *r, void *d)
{
	(void) d;
	if (r != NULL && reply_count < 8)
		snprintf(replies[reply_count++], sizeof replies[0], "%.150s", r->message);
}

static void
finish_cb(void *unused)
{
	(void) unused;
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
	timer_io->timer_free(timer_io, finish_timer);
	finish_timer = NULL;
	timer_io->destroy(timer_io);
	timer_io = NULL;
}

static void
start_calls(void)
{
	ProtobufCService *svc2;
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
	Example__HelloRequest__InputStream *in;
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
		example__hello_request__input_stream__send(in, &req);
	}
	example__hello_request__input_stream__send(in, NULL);	/* the end */

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
check_option_handling(TestLoop *l, ProtobufCService *const *services)
{
	ProtobufCService *twice[2] = { services[0], services[0] };
	ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
	ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
	ProtobufCGrpc2Server *server;
	ProtobufCGrpc2IoSystem *io = test_loop_new_io(l);

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
	test_loop_run(l);

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
	unsigned port = test_free_port();
	int rv;

	mode_ = mode;
	n_ok = n_failed = n_deadline = reply_count = 0;
	slow_closure = NULL;
	loop = test_loop_new();
	assert(loop != NULL);

	services[0] = &greeter.base;
	check_option_handling(loop, services);

	/* ---- server and client options ---- */
	so.services = services;
	so.n_services = 1;
	so.io = test_loop_new_io(loop);
	assert(so.io != NULL);
	so.on_call_done = NULL;

	co.host = "localhost";
	co.port = port;
	/* The server and client share one I/O system (one reference each). */
	co.io = so.io;
	co.on_call_done = on_call_done;

	if (mode > 0) {
		so.secure_transport = test_tls_new_server();
		assert(so.secure_transport != NULL);

		/* reference counting: the first ref claims the floating
		 * creation reference, so two refs need two releases; a system
		 * nobody took is freed by one release */
		{
			ProtobufCGrpc2SecureTransportSystem *t =
				test_tls_new_server();
			assert(t != NULL && t->ref != NULL);
			t->ref(t);
			t->ref(t);
			t->destroy(t);
			t->destroy(t);	/* freed here (ASan: no leak, no reuse) */
			t = test_tls_new_server();
			t->destroy(t);
		}

		/* mode 1: the client trusts the server's certificate; mode 2:
		 * only an unrelated one */
		co.secure_transport = test_tls_new_client(mode == 1);
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

	timer_io = test_loop_new_io(loop);
	timer_io->ref(timer_io);	/* claims the creation ref */
	assert(timer_io != NULL);
	finish_timer = timer_io->timer_new(timer_io, finish_cb, NULL);
	assert(finish_timer != NULL);
	start_calls();
	test_loop_run(loop);

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
	rv = test_loop_close(loop);		/* fails if anything leaked */
	assert(rv == 0);
}

int
main(void)
{
	run(0);
	printf("endpoints[%s/%s]: plaintext ok\n", test_backend_name(), test_tls_name());
	run(1);
	printf("endpoints[%s/%s]: TLS ok\n", test_backend_name(), test_tls_name());
	run(2);
	printf("endpoints[%s/%s]: untrusted certificate rejected ok\n", test_backend_name(), test_tls_name());
	printf("endpoints[%s/%s]: ok\n", test_backend_name(), test_tls_name());
	return 0;
}
