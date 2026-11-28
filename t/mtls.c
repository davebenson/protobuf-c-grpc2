/*
 * Mutual TLS end to end: servers that ask clients for certificates, an
 * authenticator that sees the verified identity of the client (CallInfo.peer)
 * and hands it on as the principal, and clients with and without
 * certificates. Runs on every loop backend and TLS library (tls-backend.h).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
#include "greeter.pb-c.h"
#include "free-port.h"
#include "loop-backend.h"
#include "tls-backend.h"
#include "tls-test-certs.h"

/* ---- the service: says who is calling ---- */

static void
greeter_say_hello(Example__Greeter_Service *s, const Example__HelloRequest *req,
		  Example__HelloReply_Closure cl, void *d)
{
	Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
	const char *p = protobuf_c_grpc2_current_call_principal();
	char text[160];
	(void) s;

	snprintf(text, sizeof text, "Hello, %s as %s", req->name, p ? p : "-");
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

/* ---- the authenticator: the TLS identity is the principal ---- */

static void
check(ProtobufCGrpc2ServerAuth *self, const ProtobufCGrpc2CallInfo *info,
      const ProtobufCGrpc2Header *headers, size_t n,
      ProtobufCGrpc2AuthDone done, void *done_data)
{
	(void) self; (void) headers; (void) n;
	if (info->peer == NULL)
		done(done_data, PROTOBUF_C_GRPC2_UNAUTHENTICATED,
		     "no client certificate", NULL);
	else
		done(done_data, PROTOBUF_C_GRPC2_OK, NULL, info->peer);
}

static ProtobufCGrpc2ServerAuth peer_auth = { NULL, 0, check, NULL, NULL };

/* ---- the cases ---- */

typedef struct {
	const char *what;
	int server;		/* 0: certificate required, 1: optional */
	int cert;		/* the client's: 0 none, 1 A, 2 B, 3 C */
	int want_status;
	const char *want_reply;
	ProtobufCGrpc2Client *client;
	int status, done;
	char reply[160];
} Case;

static Case cases[] = {
	{ "required, certificate A",  0, 1, PROTOBUF_C_GRPC2_OK,
	  "Hello, c0 as " TEST_CLIENT_A_IDENTITY },
	{ "required, certificate B",  0, 2, PROTOBUF_C_GRPC2_OK,
	  "Hello, c1 as " TEST_CLIENT_B_IDENTITY },
	{ "required, none",           0, 0, PROTOBUF_C_GRPC2_UNAVAILABLE, NULL },
	{ "required, untrusted",      0, 3, PROTOBUF_C_GRPC2_UNAVAILABLE, NULL },
	{ "optional, certificate A",  1, 1, PROTOBUF_C_GRPC2_OK,
	  "Hello, c4 as " TEST_CLIENT_A_IDENTITY },
	{ "optional, none: auth says no", 1, 0, PROTOBUF_C_GRPC2_UNAUTHENTICATED, NULL },
};
#define N_CASES (sizeof cases / sizeof cases[0])

static TestLoop *loop;
static ProtobufCGrpc2Server *servers[2];
static ProtobufCGrpc2IoSystem *tio;
static void *finish_timer, *watchdog;
static size_t n_done;

static void
finish_cb(void *unused)
{
	size_t i;
	(void) unused;
	for (i = 0; i < N_CASES; i++)
		protobuf_c_grpc2_client_free(cases[i].client);
	protobuf_c_grpc2_server_free(servers[0]);
	protobuf_c_grpc2_server_free(servers[1]);
	tio->timer_free(tio, watchdog);
	tio->timer_free(tio, finish_timer);
	tio->destroy(tio);
}

static void
watchdog_cb(void *unused)
{
	(void) unused;
	fprintf(stderr, "mtls: timed out (%zu of %zu calls done)\n", n_done,
		N_CASES);
	abort();
}

static void
on_done(const ProtobufCMethodDescriptor *m, int status, const char *msg, void *ud)
{
	Case *cs = ud;
	(void) m; (void) msg;
	cs->done = 1;
	cs->status = status;
	if (++n_done == N_CASES)
		tio->timer_start(tio, finish_timer, 0);
}

static void
on_reply(const Example__HelloReply *r, void *ud)
{
	Case *cs = ud;

	if (r != NULL)
		snprintf(cs->reply, sizeof cs->reply, "%s", r->message);
}

int
main(void)
{
	ProtobufCService *services[] = { NULL };
	Example__Greeter_Service greeter = EXAMPLE__GREETER__INIT(greeter_);
	unsigned ports[2];
	size_t i;

	services[0] = &greeter.base;
	loop = test_loop_new();
	assert(loop != NULL);
	tio = test_loop_new_io(loop);
	tio->ref(tio);
	finish_timer = tio->timer_new(tio, finish_cb, NULL);
	watchdog = tio->timer_new(tio, watchdog_cb, NULL);
	tio->timer_start(tio, watchdog, 30000);

	for (i = 0; i < 2; i++) {
		ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;

		ports[i] = test_free_port();
		so.services = services;
		so.n_services = 1;
		so.io = test_loop_new_io(loop);
		so.secure_transport = test_tls_new_mtls_server(
			i == 0 ? PROTOBUF_C_GRPC2_CLIENT_AUTH_REQUIRED
			       : PROTOBUF_C_GRPC2_CLIENT_AUTH_OPTIONAL,
			PROTOBUF_C_GRPC2_PEER_IDENTITY_AUTO);
		so.auth = &peer_auth;
		servers[i] = protobuf_c_grpc2_server_new(&so);
		assert(servers[i] != NULL);
		assert(protobuf_c_grpc2_server_listen(servers[i], "127.0.0.1",
						      ports[i]) == 0);
	}

	for (i = 0; i < N_CASES; i++) {
		Case *cs = &cases[i];
		ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
		Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
		ProtobufCService *svc;
		char name[16];

		co.host = "127.0.0.1";
		co.server_name = "localhost";
		co.port = ports[cs->server];
		co.io = test_loop_new_io(loop);
		co.secure_transport = test_tls_new_mtls_client(cs->cert);
		co.on_call_done = on_done;
		co.user_data = cs;
		cs->client = protobuf_c_grpc2_client_new(&co);
		assert(cs->client != NULL);
		svc = protobuf_c_grpc2_client_get_service(
			cs->client, &example__greeter__descriptor);
		snprintf(name, sizeof name, "c%zu", i);
		req.name = name;
		example__greeter__say_hello(svc, &req, on_reply, cs);
	}

	test_loop_run(loop);

	for (i = 0; i < N_CASES; i++) {
		Case *cs = &cases[i];

		if (cs->status != cs->want_status ||
		    (cs->want_reply != NULL
			     ? strcmp(cs->reply, cs->want_reply) != 0
			     : cs->reply[0] != 0))
			fprintf(stderr, "mtls: case '%s': status %s, reply '%s'\n",
				cs->what, protobuf_c_grpc2_status_name(cs->status),
				cs->reply);
		assert(cs->done && cs->status == cs->want_status);
		if (cs->want_reply != NULL)
			assert(strcmp(cs->reply, cs->want_reply) == 0);
		else
			assert(cs->reply[0] == 0);
	}
	assert(test_loop_close(loop) == 0);
	printf("mtls (%s, %s): ok\n", test_backend_name(), test_tls_name());
	return 0;
}
