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

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
#include "greeter.pb-c.h"
#include "free-port.h"
#include "loop-backend.h"

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

static TestLoop *loop;
static unsigned port;
static Example__Greeter_Service greeter;
static ProtobufCGrpc2Server *server;
static ProtobufCGrpc2Client *client;
static ProtobufCService *svc;
static ProtobufCGrpc2IoSystem *timer_io;	/* only for step_timer */
static void *step_timer;
static int phase;
static int pending;
static int probing;
static int last_status[8];
static int n_done;
static int sync_status;
static char last_reply[64];

static void step_cb(void *unused);

static void
start_server(void)
{
	ProtobufCService *services[] = { &greeter.base };
	ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;

	so.services = services;
	so.n_services = 1;
	so.io = test_loop_new_io(loop);
	assert(so.io != NULL);
	server = protobuf_c_grpc2_server_new(&so);
	assert(server != NULL);
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
		timer_io->timer_start(timer_io, step_timer, 0);
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
step_cb(void *unused)
{
	(void) unused;
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
		timer_io->timer_start(timer_io, step_timer, 350);	/* backoff is over */
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
		timer_io->timer_start(timer_io, step_timer, 150);
		break;
	case 3:
		start_server();		/* and comes back on the same port */
		phase = 4;
		timer_io->timer_start(timer_io, step_timer, 50);
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
		timer_io->timer_free(timer_io, step_timer);
		step_timer = NULL;
		timer_io->destroy(timer_io);
		timer_io = NULL;
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
	TestLoop *l = test_loop_new();
	ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
	ProtobufCGrpc2Client *c;
	ProtobufCService *s;
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
	int rv;

	co.host = "localhost";
	co.port = 1;
	co.io = test_loop_new_io(l);
	co.on_call_done = free_test_call_done;
	c = protobuf_c_grpc2_client_new(&co);
	assert(c != NULL);
	s = protobuf_c_grpc2_client_get_service(c, &example__greeter__descriptor);
	req.name = "x";
	example__greeter__say_hello(s, &req, on_reply, NULL);	/* starts connecting */
	protobuf_c_grpc2_client_free(c);	/* ... which is cancelled */
	test_loop_run(l);
	assert(free_test_done == 1 && free_test_status == PROTOBUF_C_GRPC2_UNAVAILABLE);
	rv = test_loop_close(l);
	assert(rv == 0);
	puts("client: free while connecting ok");
}

int
main(void)
{
	ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
	int rv;

	greeter = (Example__Greeter_Service) EXAMPLE__GREETER__INIT(greeter_);
	test_free_while_connecting();
	port = test_free_port();
	loop = test_loop_new();

	co.host = "127.0.0.1";
	co.port = port;
	co.io = test_loop_new_io(loop);
	co.initial_backoff_ms = 100;
	co.max_backoff_ms = 200;
	co.on_call_done = on_call_done;
	client = protobuf_c_grpc2_client_new(&co);
	assert(client != NULL);
	svc = protobuf_c_grpc2_client_get_service(client,
						  &example__greeter__descriptor);
	assert(svc != NULL);

	timer_io = test_loop_new_io(loop);
	timer_io->ref(timer_io);	/* claims the creation ref */
	assert(timer_io != NULL);
	step_timer = timer_io->timer_new(timer_io, step_cb, NULL);
	assert(step_timer != NULL);
	say("a");			/* starts connecting, which will fail */
	test_loop_run(loop);

	assert(phase == 6 && client == NULL && server == NULL);
	rv = test_loop_close(loop);
	assert(rv == 0);
	printf("client[%s]: ok\n", test_backend_name());
	return 0;
}
