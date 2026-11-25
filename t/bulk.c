/*
 * Large messages and many connections over real sockets, on every I/O
 * backend: output that does not fit in the socket buffer (partial writes,
 * write-readiness), a message bigger than one read, and many clients
 * connected to one server at once.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
#include "greeter.pb-c.h"
#include "loop-backend.h"

#define BIG (3u << 20)			/* 3 MiB: well above any socket buffer */
#define N_CLIENTS 24
#define MEDIUM (200u << 10)

static void
greeter_say_hello(Example__Greeter_Service *s, const Example__HelloRequest *req,
		  Example__HelloReply_Closure cl, void *d)
{
	Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
	size_t n = strlen(req->name);
	char *text = malloc(n + 16);
	(void) s;
	assert(text != NULL);
	snprintf(text, n + 16, "Hello, %s!", req->name);
	reply.message = text;
	cl(&reply, d);
	free(text);
}

static void
greeter_say_hello_many(Example__Greeter_Service *s, const Example__HelloRequest *r,
		       Example__HelloReply_Closure cl, void *d)
{
	(void) s; (void) r;
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

static TestLoop *loop;
static ProtobufCGrpc2IoSystem *timer_io;
static void *finish_timer;
static ProtobufCGrpc2Server *server;
static ProtobufCGrpc2Client *clients[N_CLIENTS + 1];
static int pending, n_ok, n_wrong;
typedef struct { size_t expect; } Expect;
static Expect expects[N_CLIENTS + 4];
static int n_calls;

static void
on_reply(const Example__HelloReply *r, void *d)
{
	Expect *e = d;

	if (r == NULL)
		return;
	if (strlen(r->message) == e->expect &&
	    strncmp(r->message, "Hello, ", 7) == 0)
		n_ok++;
	else
		n_wrong++;
}

static void
finish_cb(void *unused)
{
	int i;
	(void) unused;
	for (i = 0; i <= N_CLIENTS; i++) {
		protobuf_c_grpc2_client_free(clients[i]);
		clients[i] = NULL;
	}
	protobuf_c_grpc2_server_free(server);
	server = NULL;
	timer_io->timer_free(timer_io, finish_timer);
	timer_io->destroy(timer_io);
}

static void
on_call_done(const ProtobufCMethodDescriptor *m, int status, const char *msg,
	     void *ud)
{
	(void) m; (void) ud;
	if (status != PROTOBUF_C_GRPC2_OK)
		fprintf(stderr, "call failed: %d %s\n", status, msg ? msg : "");
	assert(status == PROTOBUF_C_GRPC2_OK);
	if (--pending == 0)
		timer_io->timer_start(timer_io, finish_timer, 0);
}

static ProtobufCGrpc2Client *
new_client(unsigned port, size_t max_msg)
{
	ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
	ProtobufCGrpc2Client *c;

	co.host = "127.0.0.1";
	co.port = port;
	co.io = test_loop_new_io(loop);
	co.max_message_size = max_msg;
	co.on_call_done = on_call_done;
	assert(co.io != NULL);
	c = protobuf_c_grpc2_client_new(&co);
	assert(c != NULL);
	return c;
}

static void
call(int i, size_t name_len)
{
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
	ProtobufCService *svc = protobuf_c_grpc2_client_get_service(
		clients[i], &example__greeter__descriptor);
	char *name = malloc(name_len + 1);

	assert(name != NULL);
	memset(name, 'a' + i % 26, name_len);
	name[name_len] = 0;
	req.name = name;
	expects[n_calls].expect = name_len + 8;	/* "Hello, " + name + "!" */
	pending++;
	example__greeter__say_hello(svc, &req, on_reply, &expects[n_calls++]);
	free(name);			/* the request is serialized by now */
}

int
main(void)
{
	Example__Greeter_Service greeter = EXAMPLE__GREETER__INIT(greeter_);
	ProtobufCService *services[] = { &greeter.base };
	ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
	unsigned port = 20000 + ((unsigned) getpid() * 7 + 5) % 20000;
	int i, rv;

	loop = test_loop_new();
	assert(loop != NULL);
	so.services = services;
	so.n_services = 1;
	so.io = test_loop_new_io(loop);
	so.max_message_size = 8u << 20;
	server = protobuf_c_grpc2_server_new(&so);
	assert(server != NULL);
	assert(protobuf_c_grpc2_server_listen(server, "127.0.0.1", port) == 0);

	timer_io = test_loop_new_io(loop);
	timer_io->ref(timer_io);	/* claims the creation ref */
	finish_timer = timer_io->timer_new(timer_io, finish_cb, NULL);
	assert(finish_timer != NULL);

	for (i = 0; i < N_CLIENTS; i++)
		clients[i] = new_client(port, 0);
	clients[N_CLIENTS] = new_client(port, 8u << 20);
	for (i = 0; i < N_CLIENTS; i++)
		call(i, MEDIUM);
	call(N_CLIENTS, BIG);			/* one connection, one huge call */
	for (i = 0; i < 3; i++)			/* ... and calls queued behind it */
		call(N_CLIENTS, 1000 * (unsigned) (i + 1));

	test_loop_run(loop);

	assert(n_wrong == 0);
	assert(n_ok == N_CLIENTS + 4);
	assert(pending == 0 && server == NULL);
	rv = test_loop_close(loop);
	assert(rv == 0);
	printf("bulk[%s]: ok\n", test_backend_name());
	return 0;
}
