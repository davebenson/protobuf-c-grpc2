/*
 * A thread safe client: worker threads make unary, server-streaming and
 * client-streaming calls through one client while the loop runs in the main
 * thread, which also hosts the server. Every closure and on_call_done must
 * run in the main thread.
 */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
#include "greeter.pb-c.h"
#include "free-port.h"
#include "loop-backend.h"

#define N_THREADS 4
#define N_ROUNDS 25

/* ---- the service ---- */

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
	ss->base.send = sum_send;
	ss->base.input_descriptor = &example__hello_request__descriptor;
	ss->cl = cl;
	ss->d = d;
	if (first != NULL)
		sum_send(&ss->base, &first->base);
	return &ss->base;
}

/* ---- the test ---- */

static pthread_t main_thread;
static TestLoop *loop;
static Example__Greeter_Service greeter;
static ProtobufCGrpc2Server *server;
static ProtobufCGrpc2Client *client;
static ProtobufCGrpc2IoSystem *timer_io;
static void *finish_timer, *watchdog;

static const int total_calls = N_THREADS * N_ROUNDS * 3 + 1;
/* calls started and not yet done: the workers keep it below the number of
 * streams a connection allows at once (calls past that fail, they are not
 * queued) */
static atomic_int outstanding;
#define MAX_OUTSTANDING 60

static int n_done, n_failed, n_bad, n_wrong_thread;
static int n_unary, n_many, n_sum;

static void
in_main_thread(void)
{
	if (!pthread_equal(pthread_self(), main_thread))
		n_wrong_thread++;
}

static void
finish_cb(void *unused)
{
	(void) unused;
	protobuf_c_grpc2_client_free(client);
	client = NULL;
	protobuf_c_grpc2_server_free(server);
	server = NULL;
	timer_io->timer_free(timer_io, watchdog);
	timer_io->timer_free(timer_io, finish_timer);
	timer_io->destroy(timer_io);
}

static void
watchdog_cb(void *unused)
{
	(void) unused;
	fprintf(stderr, "threads: timed out (%d of %d calls done)\n", n_done,
		total_calls);
	abort();
}

static void
on_done(const ProtobufCMethodDescriptor *m, int status, const char *msg,
	void *ud)
{
	(void) m; (void) msg; (void) ud;
	in_main_thread();
	atomic_fetch_sub(&outstanding, 1);
	if (status != PROTOBUF_C_GRPC2_OK) {
		if (n_failed++ < 3)
			fprintf(stderr, "call failed: %s %s (%s)\n", protobuf_c_grpc2_status_name(status), msg ? msg : "", m ? m->name : "?");
	}
	if (++n_done == total_calls)
		timer_io->timer_start(timer_io, finish_timer, 0);
}

/* the reply closures: text is checked against what the worker sent */
static void
on_hello(const Example__HelloReply *r, void *d)
{
	in_main_thread();
	if (r == NULL)
		return;
	n_unary++;
	if (strncmp(r->message, "Hello, w", 8) != 0 || d != (void *) 1)
		n_bad++;
}

static int many_seen;
static void
on_many(const Example__HelloReply *r, void *d)
{
	in_main_thread();
	(void) d;
	if (r == NULL) {
		n_many++;
		return;
	}
	many_seen++;
}

static void
on_sum(const Example__HelloReply *r, void *d)
{
	in_main_thread();
	(void) d;
	if (r == NULL)
		return;
	n_sum++;
	if (strcmp(r->message, "total=10") != 0)
		n_bad++;
}

static ProtobufCService *svc;	/* obtained on the loop thread */

static void *
worker(void *arg)
{
	long w = (long) arg;
	int i, k;

	for (i = 0; i < N_ROUNDS; i++) {
		Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
		Example__HelloRequest__InputStream *in;
		char name[32];

		while (atomic_load(&outstanding) > MAX_OUTSTANDING - 3)
			sched_yield();
		atomic_fetch_add(&outstanding, 3);
		snprintf(name, sizeof name, "w%ld-%d", w, i);
		req.name = name;		/* copied: the stack is reused */
		example__greeter__say_hello(svc, &req, on_hello, (void *) 1);
		req.count = 3;
		example__greeter__say_hello_many(svc, &req, on_many, NULL);
		in = example__greeter__collect(svc, NULL, on_sum, NULL);
		assert(in != NULL);
		for (k = 1; k <= 4; k++) {
			req.count = k;
			example__hello_request__input_stream__send(in, &req);
		}
		example__hello_request__input_stream__send(in, NULL);
		memset(name, 'x', sizeof name - 1);	/* must not matter now */
	}
	return NULL;
}

int
main(void)
{
	ProtobufCService *services[] = { &greeter.base };
	ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
	ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
	pthread_t th[N_THREADS];
	unsigned port = test_free_port();
	long i;

	main_thread = pthread_self();
	greeter = (Example__Greeter_Service) EXAMPLE__GREETER__INIT(greeter_);

	loop = test_loop_new();
	assert(loop != NULL);
	so.services = services;
	so.n_services = 1;
	so.io = test_loop_new_io(loop);
	server = protobuf_c_grpc2_server_new(&so);
	assert(server != NULL);
	assert(protobuf_c_grpc2_server_listen(server, "127.0.0.1", port) == 0);

	/* a system that cannot post cannot serve a thread safe client */
	{
		ProtobufCGrpc2ClientOptions bad = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
		ProtobufCGrpc2IoSystem *real = test_loop_new_io(loop);
		ProtobufCGrpc2IoSystem fake = *real;

		fake.post = NULL;
		bad.host = "127.0.0.1";
		bad.port = port;
		bad.io = &fake;
		bad.thread_safe = 1;
		assert(protobuf_c_grpc2_client_new(&bad) == NULL);
		real->ref(real);	/* claim the creation reference, then drop it */
		real->destroy(real);
	}

	co.host = "127.0.0.1";
	co.port = port;
	co.io = test_loop_new_io(loop);
	co.thread_safe = 1;
	co.on_call_done = on_done;
	client = protobuf_c_grpc2_client_new(&co);
	assert(client != NULL);
	svc = protobuf_c_grpc2_client_get_service(client,
						  &example__greeter__descriptor);
	assert(svc != NULL);

	timer_io = test_loop_new_io(loop);
	timer_io->ref(timer_io);
	finish_timer = timer_io->timer_new(timer_io, finish_cb, NULL);
	watchdog = timer_io->timer_new(timer_io, watchdog_cb, NULL);
	timer_io->timer_start(timer_io, watchdog, 30000);	/* also keeps the loop running */

	/* a call from the loop thread goes straight through */
	req.name = "w-main";
	atomic_fetch_add(&outstanding, 1);
	example__greeter__say_hello(svc, &req, on_hello, (void *) 1);

	for (i = 0; i < N_THREADS; i++)
		assert(pthread_create(&th[i], NULL, worker, (void *) i) == 0);
	test_loop_run(loop);
	for (i = 0; i < N_THREADS; i++)
		assert(pthread_join(th[i], NULL) == 0);

	assert(n_done == total_calls);
	if (n_failed || n_bad || n_wrong_thread)
		fprintf(stderr, "failed=%d bad=%d wrong_thread=%d\n", n_failed, n_bad, n_wrong_thread);
	assert(n_failed == 0 && n_bad == 0 && n_wrong_thread == 0);
	assert(n_unary == N_THREADS * N_ROUNDS + 1);
	assert(n_many == N_THREADS * N_ROUNDS && many_seen == 3 * n_many);
	assert(n_sum == N_THREADS * N_ROUNDS);
	assert(test_loop_close(loop) == 0);
	printf("threads (%s): ok\n", test_backend_name());
	return 0;
}
