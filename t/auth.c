/*
 * Authentication: one server with a bearer authenticator, and eleven clients
 * with different credentials making one call each at the same time. Covers
 * answers that come at once (the fast path) and later, rejection, request data
 * that arrives while the verdict is pending, failing credentials, and
 * credentials that answer only after the call's deadline.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
#include "greeter.pb-c.h"
#include "free-port.h"
#include "loop-backend.h"

/* ---- timers for "later" ---- */

static TestLoop *loop;
static ProtobufCGrpc2IoSystem *tio;		/* only for the timers */

typedef struct Later {
	void *timer;
	void (*fn)(void *);
	void *arg;
} Later;

static void
later_fire(void *p)
{
	Later *l = p;
	void (*fn)(void *) = l->fn;
	void *arg = l->arg;

	tio->timer_free(tio, l->timer);
	free(l);
	fn(arg);
}

static void
later(unsigned ms, void (*fn)(void *), void *arg)
{
	Later *l = calloc(1, sizeof(*l));

	l->fn = fn;
	l->arg = arg;
	l->timer = tio->timer_new(tio, later_fire, l);
	tio->timer_start(tio, l->timer, ms);
}

/* ---- the service: answers with the caller's principal ---- */

static int handler_calls;

static const char *
principal_or_dash(void)
{
	const char *p = protobuf_c_grpc2_current_call_principal();

	return p != NULL ? p : "-";
}

static void
greeter_say_hello(Example__Greeter_Service *s, const Example__HelloRequest *req,
		  Example__HelloReply_Closure cl, void *d)
{
	Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
	char text[128];
	(void) s;
	handler_calls++;
	snprintf(text, sizeof text, "Hello, %s as %s", req->name, principal_or_dash());
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

typedef struct {
	ProtobufCInputStream base;
	int total;
	char who[32];
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
		char text[96];

		snprintf(text, sizeof text, "total=%d as %s", ss->total, ss->who);
		reply.message = text;
		ss->cl(&reply, ss->d);
		free(ss);
	}
	return 1;
}

static ProtobufCInputStream *
greeter_collect(Example__Greeter_Service *s, const Example__HelloRequest *first,
		Example__HelloReply_Closure cl, void *d)
{
	SumStream *ss = calloc(1, sizeof(*ss));
	(void) s;
	handler_calls++;
	ss->base.send = sum_send;
	ss->base.input_descriptor = &example__hello_request__descriptor;
	snprintf(ss->who, sizeof ss->who, "%s", principal_or_dash());
	ss->cl = cl;
	ss->d = d;
	if (first != NULL)
		sum_send(&ss->base, &first->base);
	return &ss->base;
}

/* ---- the server's side: a bearer verifier ---- */

typedef struct {
	ProtobufCGrpc2AuthDone done;
	void *done_data;
	int status;
	const char *principal;
} Verdict;

static void
verdict_fire(void *p)
{
	Verdict *v = p;

	v->done(v->done_data, v->status, v->status ? "no" : NULL, v->principal);
	free(v);
}

static int verify_calls;

static void
verify(void *ud, const char *token, const ProtobufCGrpc2CallInfo *info,
       ProtobufCGrpc2AuthDone done, void *done_data)
{
	Verdict *v;
	(void) ud;
	verify_calls++;
	assert(info->method != NULL && info->service != NULL);
	if (strcmp(token, "fast") == 0) {			/* at once */
		done(done_data, PROTOBUF_C_GRPC2_OK, NULL, "alice");
		return;
	}
	if (strcmp(token, "bad") == 0) {			/* at once */
		done(done_data, PROTOBUF_C_GRPC2_UNAUTHENTICATED, "bad token", NULL);
		return;
	}
	/* "slow" and "slowbad": later, so that the request data that follows
	 * the headers has to be held */
	v = calloc(1, sizeof(*v));
	v->done = done;
	v->done_data = done_data;
	if (strcmp(token, "slow") == 0) {
		v->status = PROTOBUF_C_GRPC2_OK;
		v->principal = "bob";
	} else {
		v->status = PROTOBUF_C_GRPC2_PERMISSION_DENIED;
	}
	later(40, verdict_fire, v);
}

/* ---- a client's side: custom credentials ---- */

enum { CR_FAST, CR_SLOW, CR_FAIL_NOW, CR_FAIL_LATER, CR_LATE };

typedef struct {
	ProtobufCGrpc2CallCredentials base;	/* must be first */
	int mode;
} Creds;

typedef struct {
	ProtobufCGrpc2CredentialsDone done;
	void *done_data;
	int mode;
} CredAnswer;

static int late_fired;
static void maybe_finish(void);

static void
cred_answer_fire(void *p)
{
	CredAnswer *a = p;
	ProtobufCGrpc2Header h = { "authorization", "Bearer fast" };

	if (a->mode == CR_FAIL_LATER)
		a->done(a->done_data, PROTOBUF_C_GRPC2_UNAVAILABLE,
			"token service down", NULL, 0);
	else
		a->done(a->done_data, PROTOBUF_C_GRPC2_OK, NULL, &h, 1);
	if (a->mode == CR_LATE) {
		late_fired = 1;
		maybe_finish();
	}
	free(a);
}

static void
creds_get(ProtobufCGrpc2CallCredentials *self, ProtobufCGrpc2IoSystem *io,
	  const ProtobufCGrpc2CallInfo *info, ProtobufCGrpc2CredentialsDone done,
	  void *done_data)
{
	Creds *cr = (Creds *) self;
	ProtobufCGrpc2Header h = { "authorization", "Bearer fast" };
	CredAnswer *a;

	assert(io != NULL && info->method != NULL);
	switch (cr->mode) {
	case CR_FAST:
		done(done_data, PROTOBUF_C_GRPC2_OK, NULL, &h, 1);
		return;
	case CR_FAIL_NOW:
		done(done_data, PROTOBUF_C_GRPC2_UNAVAILABLE, "no token", NULL, 0);
		return;
	}
	a = calloc(1, sizeof(*a));
	a->done = done;
	a->done_data = done_data;
	a->mode = cr->mode;
	if (cr->mode == CR_SLOW) {
		/* "slow" selects the slow verifier answer on the server */
		a->mode = CR_SLOW;
		later(30, cred_answer_fire, a);
	} else if (cr->mode == CR_FAIL_LATER) {
		later(30, cred_answer_fire, a);
	} else {
		later(400, cred_answer_fire, a);	/* after the deadline */
	}
}

/* ---- the cases ---- */

typedef struct {
	const char *what;
	int use_stock;			/* stock bearer credentials, else custom */
	const char *token;		/* stock: the token; NULL: no credentials */
	int mode;			/* custom: a CR_* */
	int streaming;			/* Collect, else SayHello */
	unsigned timeout_ms;
	int want_status;
	const char *want_reply;		/* NULL: no reply expected */
	/* run-time */
	ProtobufCGrpc2Client *client;
	Creds creds;
	int status;
	int done;
	char reply[128];
} Case;

static Case cases[] = {
	{ "fast ok",           1, "fast",    0, 0, 0,   PROTOBUF_C_GRPC2_OK, "Hello, c0 as alice" },
	{ "slow ok",           1, "slow",    0, 0, 0,   PROTOBUF_C_GRPC2_OK, "Hello, c1 as bob" },
	{ "slow, held data",   1, "slow",    0, 1, 0,   PROTOBUF_C_GRPC2_OK, "total=6 as bob" },
	{ "rejected at once",  1, "bad",     0, 0, 0,   PROTOBUF_C_GRPC2_UNAUTHENTICATED, NULL },
	{ "rejected later",    1, "slowbad", 0, 1, 0,   PROTOBUF_C_GRPC2_PERMISSION_DENIED, NULL },
	{ "no credentials",    1, NULL,      0, 0, 0,   PROTOBUF_C_GRPC2_UNAUTHENTICATED, NULL },
	{ "creds: later",      0, NULL,  CR_SLOW, 0, 0, PROTOBUF_C_GRPC2_OK, "Hello, c6 as alice" },
	{ "creds: at once",    0, NULL,  CR_FAST, 1, 0, PROTOBUF_C_GRPC2_OK, "total=6 as alice" },
	{ "creds fail at once",0, NULL,  CR_FAIL_NOW, 0, 0, PROTOBUF_C_GRPC2_UNAVAILABLE, NULL },
	{ "creds fail later",  0, NULL,  CR_FAIL_LATER, 1, 0, PROTOBUF_C_GRPC2_UNAVAILABLE, NULL },
	{ "creds after deadline", 0, NULL, CR_LATE, 0, 100, PROTOBUF_C_GRPC2_DEADLINE_EXCEEDED, NULL },
};
#define N_CASES (sizeof cases / sizeof cases[0])

static ProtobufCGrpc2Server *server;
static ProtobufCGrpc2ServerAuth *server_auth;
static void *finish_timer, *watchdog;
static size_t n_done;
static int finished;

static void
finish_cb(void *unused)
{
	size_t i;
	(void) unused;
	for (i = 0; i < N_CASES; i++)
		protobuf_c_grpc2_client_free(cases[i].client);
	protobuf_c_grpc2_server_free(server);
	tio->timer_free(tio, watchdog);
	tio->timer_free(tio, finish_timer);
	tio->destroy(tio);
}

static void
maybe_finish(void)
{
	if (!finished && n_done == N_CASES && late_fired) {
		finished = 1;
		tio->timer_start(tio, finish_timer, 0);
	}
}

static void
watchdog_cb(void *unused)
{
	size_t i;
	(void) unused;
	fprintf(stderr, "auth: timed out; done: ");
	for (i = 0; i < N_CASES; i++)
		fprintf(stderr, "%d", cases[i].done);
	fprintf(stderr, "\n");
	abort();
}

static void
on_done(const ProtobufCMethodDescriptor *m, int status, const char *msg, void *ud)
{
	Case *cs = ud;
	(void) m; (void) msg;
	assert(!cs->done);
	cs->done = 1;
	cs->status = status;
	n_done++;
	maybe_finish();
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
	ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
	ProtobufCGrpc2BearerAuthOptions bo = PROTOBUF_C_GRPC2_BEARER_AUTH_OPTIONS_INIT;
	unsigned port = test_free_port();
	size_t i;

	services[0] = &greeter.base;
	loop = test_loop_new();
	assert(loop != NULL);
	tio = test_loop_new_io(loop);
	tio->ref(tio);
	finish_timer = tio->timer_new(tio, finish_cb, NULL);
	watchdog = tio->timer_new(tio, watchdog_cb, NULL);
	tio->timer_start(tio, watchdog, 30000);

	/* a bad option set is refused */
	{
		ProtobufCGrpc2BearerAuthOptions none = PROTOBUF_C_GRPC2_BEARER_AUTH_OPTIONS_INIT;
		ProtobufCGrpc2BearerCredentialsOptions cnone =
			PROTOBUF_C_GRPC2_BEARER_CREDENTIALS_OPTIONS_INIT;

		assert(protobuf_c_grpc2_server_auth_new_bearer(&none) == NULL);
		assert(protobuf_c_grpc2_call_credentials_new_bearer(&cnone) == NULL);
	}

	bo.verify = verify;
	server_auth = protobuf_c_grpc2_server_auth_new_bearer(&bo);
	assert(server_auth != NULL);
	so.services = services;
	so.n_services = 1;
	so.io = test_loop_new_io(loop);
	so.auth = server_auth;
	server = protobuf_c_grpc2_server_new(&so);
	assert(server != NULL);
	assert(protobuf_c_grpc2_server_listen(server, "127.0.0.1", port) == 0);

	for (i = 0; i < N_CASES; i++) {
		Case *cs = &cases[i];
		ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
		ProtobufCService *svc;
		Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
		char name[16];

		co.host = "127.0.0.1";
		co.port = port;
		co.io = test_loop_new_io(loop);
		co.on_call_done = on_done;
		co.user_data = cs;
		/* no TLS here: say so */
		co.allow_insecure_credentials = 1;
		if (cs->use_stock && cs->token != NULL) {
			ProtobufCGrpc2BearerCredentialsOptions bc =
				PROTOBUF_C_GRPC2_BEARER_CREDENTIALS_OPTIONS_INIT;

			bc.token = cs->token;
			co.credentials = protobuf_c_grpc2_call_credentials_new_bearer(&bc);
			assert(co.credentials != NULL);
		} else if (!cs->use_stock) {
			cs->creds.base.get = creds_get;
			cs->creds.mode = cs->mode;
			co.credentials = &cs->creds.base;
		}
		cs->client = protobuf_c_grpc2_client_new(&co);
		assert(cs->client != NULL);
		svc = protobuf_c_grpc2_client_get_service(cs->client,
							  &example__greeter__descriptor);
		if (cs->timeout_ms)
			protobuf_c_grpc2_client_service_set_timeout_ms(svc, cs->timeout_ms);

		snprintf(name, sizeof name, "c%zu", i);
		req.name = name;
		if (!cs->streaming) {
			example__greeter__say_hello(svc, &req, on_reply, cs);
		} else {
			Example__HelloRequest__InputStream *in =
				example__greeter__collect(svc, NULL, on_reply, cs);
			int k;

			assert(in != NULL);
			/* sent before any credentials or verdict can have come */
			for (k = 1; k <= 3; k++) {
				req.count = k;
				example__hello_request__input_stream__send(in, &req);
			}
			example__hello_request__input_stream__send(in, NULL);
		}
	}

	/* credentials without TLS are refused, unless said to be fine */
	{
		ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
		ProtobufCGrpc2IoSystem *io = test_loop_new_io(loop);

		co.host = "127.0.0.1";
		co.port = port;
		co.io = io;
		co.credentials = &cases[6].creds.base;
		assert(protobuf_c_grpc2_client_new(&co) == NULL);
		io->ref(io);		/* nothing was taken: claim and drop it */
		io->destroy(io);
	}

	test_loop_run(loop);

	for (i = 0; i < N_CASES; i++) {
		Case *cs = &cases[i];

		if (cs->status != cs->want_status ||
		    (cs->want_reply != NULL
			     ? strcmp(cs->reply, cs->want_reply) != 0
			     : cs->reply[0] != 0))
			fprintf(stderr, "auth: case '%s': status %s (want %s), reply '%s'\n",
				cs->what, protobuf_c_grpc2_status_name(cs->status),
				protobuf_c_grpc2_status_name(cs->want_status), cs->reply);
		assert(cs->done);
		assert(cs->status == cs->want_status);
		if (cs->want_reply != NULL)
			assert(strcmp(cs->reply, cs->want_reply) == 0);
		else
			assert(cs->reply[0] == 0);
	}
	/* rejected calls never reached a handler */
	assert(handler_calls == 5);
	assert(test_loop_close(loop) == 0);
	printf("auth (%s): ok\n", test_backend_name());
	return 0;
}
