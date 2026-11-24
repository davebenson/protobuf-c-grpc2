/*
 * Client connection management test (cleartext): connect failure, backoff,
 * lazy reconnect, calls queued while connecting, and reconnecting after the
 * server goes away and comes back.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2-builder.h"
#include "protobuf-c-grpc2/protobuf-c-grpc2-uv.h"
#include "greeter.pb-c.h"

/* ---- a minimal Greeter ---- */

static void
greeter_say_hello(Example__Greeter_Service *s, const Example__HelloRequest *req,
		  Example__HelloReply_Closure cl, void *d)
{
	Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
	char text[128];
	(void) s;
	snprintf(text, sizeof text, "Hello, %s!", req->name);
	reply.message = text;
	cl(&reply, d);
}

static void
greeter_say_hello_many(Example__Greeter_Service *s,
		       const Example__HelloRequest *req,
		       Example__HelloReply_Closure cl, void *d)
{
	(void) s; (void) req;
	cl(NULL, d);
}

static ProtobufCInputStream *
greeter_collect(Example__Greeter_Service *s, const Example__HelloRequest *first,
		Example__HelloReply_Closure cl, void *d)
{
	(void) s; (void) first;
	cl(NULL, d);
	return NULL;
}

/* ---- the test ---- */

static uv_loop_t *loop;
static unsigned port;
static Example__Greeter_Service greeter;
static ProtobufCGrpc2Server *server;
static ProtobufCGrpc2Client *client;
static ProtobufCService *svc;
static uv_timer_t step_timer;
static int phase;
static int pending;
static int probing;
static int last_status[8];
static int n_done;
static int sync_status;
static char last_reply[64];

static void step_cb(uv_timer_t *t);

static void
start_server(void)
{
	ProtobufCGrpc2ServerBuilder *b = protobuf_c_grpc2_server_builder_new();

	assert(protobuf_c_grpc2_server_builder_add_service(b, &greeter.base) == 0);
	assert(protobuf_c_grpc2_server_builder_use_uv(b, loop) == 0);
	server = protobuf_c_grpc2_server_from_builder(b);
	assert(server != NULL);
	protobuf_c_grpc2_server_builder_free(b);
	assert(protobuf_c_grpc2_server_listen(server, "127.0.0.1", port) == 0);
}

static void
on_call_done(const ProtobufCMethodDescriptor *m, int status, const char *msg,
	     void *ud)
{
	(void) m; (void) msg; (void) ud;
	if (probing) {
		sync_status = status;
		return;
	}
	if (n_done < 8)
		last_status[n_done] = status;
	n_done++;
	if (--pending == 0)
		uv_timer_start(&step_timer, step_cb, 0, 0);
}

static void
on_reply(const Example__HelloReply *r, void *d)
{
	(void) d;
	if (r != NULL)
		snprintf(last_reply, sizeof last_reply, "%s", r->message);
}

static void
say(const char *name)
{
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;

	req.name = (char *) name;
	pending++;
	example__greeter__say_hello(svc, &req, on_reply, NULL);
}

static void
step_cb(uv_timer_t *t)
{
	(void) t;
	switch (phase) {
	case 0:
		/* A: nobody is listening, so connecting fails */
		assert(n_done == 1 && last_status[0] == PROTOBUF_C_GRPC2_UNAVAILABLE);
		{
			/* B: the client is now backing off: this fails at once */
			Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
			req.name = "b";
			probing = 1;
			sync_status = -1;
			example__greeter__say_hello(svc, &req, on_reply, NULL);
			probing = 0;
			assert(sync_status == PROTOBUF_C_GRPC2_UNAVAILABLE);
		}
		puts("client: connect failure and backoff ok");
		start_server();
		phase = 1;
		uv_timer_start(&step_timer, step_cb, 350, 0);	/* backoff is over */
		break;
	case 1:
		/* C: lazy reconnect; two calls queue behind one connection */
		say("c");
		say("c2");
		phase = 2;
		break;
	case 2:
		assert(n_done == 3);
		assert(last_status[1] == PROTOBUF_C_GRPC2_OK);
		assert(last_status[2] == PROTOBUF_C_GRPC2_OK);
		assert(strcmp(last_reply, "Hello, c2!") == 0 ||
		       strcmp(last_reply, "Hello, c!") == 0);
		puts("client: lazy connect and queued calls ok");
		/* the server goes away; the client should notice */
		protobuf_c_grpc2_server_free(server);
		server = NULL;
		phase = 3;
		uv_timer_start(&step_timer, step_cb, 150, 0);
		break;
	case 3:
		start_server();		/* and comes back on the same port */
		phase = 4;
		uv_timer_start(&step_timer, step_cb, 50, 0);
		break;
	case 4:
		/* D: reconnects at once: a lost working connection is not
		 * a failed attempt, so there is no backoff */
		say("d");
		phase = 5;
		break;
	case 5:
		assert(n_done == 4 && last_status[3] == PROTOBUF_C_GRPC2_OK);
		assert(strcmp(last_reply, "Hello, d!") == 0);
		puts("client: reconnect after server restart ok");
		protobuf_c_grpc2_client_free(client);
		client = NULL;
		protobuf_c_grpc2_server_free(server);
		server = NULL;
		uv_close((uv_handle_t *) &step_timer, NULL);
		phase = 6;
		break;
	}
}

/* Freeing a client whose connection attempt is still in flight: the attempt
 * is cancelled, the queued call completes once with UNAVAILABLE, and every
 * libuv handle is released. */
static int free_test_done;
static int free_test_status = -1;

static void
free_test_call_done(const ProtobufCMethodDescriptor *m, int status,
		    const char *msg, void *ud)
{
	(void) m; (void) msg; (void) ud;
	free_test_done++;
	free_test_status = status;
}

static void
test_free_while_connecting(void)
{
	uv_loop_t *l = uv_loop_new();
	ProtobufCGrpc2ClientBuilder *cb = protobuf_c_grpc2_client_builder_new();
	ProtobufCGrpc2ConnOptions o;
	ProtobufCGrpc2Client *c;
	ProtobufCService *s;
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
	int rv;

	assert(protobuf_c_grpc2_client_builder_set_target(cb, "localhost", 1) == 0);
	assert(protobuf_c_grpc2_client_builder_use_uv(cb, l) == 0);
	memset(&o, 0, sizeof o);
	o.on_call_done = free_test_call_done;
	assert(protobuf_c_grpc2_client_builder_set_options(cb, &o) == 0);
	c = protobuf_c_grpc2_client_from_builder(cb);
	protobuf_c_grpc2_client_builder_free(cb);
	s = protobuf_c_grpc2_client_get_service(c, &example__greeter__descriptor);
	req.name = "x";
	example__greeter__say_hello(s, &req, on_reply, NULL);	/* starts connecting */
	protobuf_c_grpc2_client_free(c);	/* ... which is cancelled */
	uv_run(l, UV_RUN_DEFAULT);
	assert(free_test_done == 1 && free_test_status == PROTOBUF_C_GRPC2_UNAVAILABLE);
	rv = uv_loop_close(l);
	assert(rv == 0);
	free(l);
	puts("client: free while connecting ok");
}

int
main(void)
{
	ProtobufCGrpc2ClientBuilder *cb;
	ProtobufCGrpc2ConnOptions copts;
	int rv;

	greeter = (Example__Greeter_Service) EXAMPLE__GREETER__INIT(greeter_);
	test_free_while_connecting();
	port = 20000 + ((unsigned) getpid() * 7 + 3) % 20000;
	loop = uv_loop_new();

	cb = protobuf_c_grpc2_client_builder_new();
	assert(protobuf_c_grpc2_client_builder_set_target(cb, "127.0.0.1", port) == 0);
	assert(protobuf_c_grpc2_client_builder_use_uv(cb, loop) == 0);
	protobuf_c_grpc2_client_builder_set_backoff(cb, 100, 200);
	memset(&copts, 0, sizeof copts);
	copts.on_call_done = on_call_done;
	assert(protobuf_c_grpc2_client_builder_set_options(cb, &copts) == 0);
	client = protobuf_c_grpc2_client_from_builder(cb);
	assert(client != NULL);
	protobuf_c_grpc2_client_builder_free(cb);
	svc = protobuf_c_grpc2_client_get_service(client,
						  &example__greeter__descriptor);
	assert(svc != NULL);

	uv_timer_init(loop, &step_timer);
	say("a");			/* starts connecting, which will fail */
	uv_run(loop, UV_RUN_DEFAULT);

	assert(phase == 6 && client == NULL && server == NULL);
	rv = uv_loop_close(loop);
	assert(rv == 0);
	free(loop);
	puts("client: ok");
	return 0;
}
