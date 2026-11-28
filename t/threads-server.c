/*
 * A thread safe server: the handlers hand each request to a pool of worker
 * threads, which answer through the response closure from there. The loop, the
 * server and a client run in the main thread.
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
#include "greeter.pb-c.h"
#include "free-port.h"
#include "loop-backend.h"

#define N_WORKERS 4
#define N_ROUNDS 40

/* ---- the worker pool ---- */

typedef struct Job Job;
struct Job {
	Job *next;
	int kind;			/* 0 hello, 1 many, 2 sum */
	char name[48];
	int count;
	Example__HelloReply_Closure cl;
	void *d;
};

static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static Job *queue, *queue_tail;
static int stopping;

static void
enqueue(Job *j)
{
	pthread_mutex_lock(&qmu);
	if (queue_tail != NULL)
		queue_tail->next = j;
	else
		queue = j;
	queue_tail = j;
	pthread_cond_signal(&qcv);
	pthread_mutex_unlock(&qmu);
}

static void *
worker(void *arg)
{
	(void) arg;
	for (;;) {
		Job *j;
		int i;

		pthread_mutex_lock(&qmu);
		while (queue == NULL && !stopping)
			pthread_cond_wait(&qcv, &qmu);
		if (queue == NULL) {
			pthread_mutex_unlock(&qmu);
			return NULL;
		}
		j = queue;
		queue = j->next;
		if (queue == NULL)
			queue_tail = NULL;
		pthread_mutex_unlock(&qmu);

		/* answer from this thread; the closure is the library's */
		if (j->kind == 0) {
			Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
			char text[96];

			snprintf(text, sizeof text, "Hello, %s!", j->name);
			reply.message = text;
			j->cl(&reply, j->d);
		} else if (j->kind == 1) {
			for (i = 1; i <= j->count; i++) {
				Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
				char text[32];

				snprintf(text, sizeof text, "#%d", i);
				reply.message = text;
				j->cl(&reply, j->d);
			}
			j->cl(NULL, j->d);
		} else {
			Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
			char text[32];

			snprintf(text, sizeof text, "total=%d", j->count);
			reply.message = text;
			j->cl(&reply, j->d);
		}
		free(j);
	}
}

/* ---- the service (handlers run on the loop thread) ---- */

static pthread_t main_thread;
static Job *late_job;
static int finished;	/* the client is freed */
static int n_handler_wrong_thread;

static void
check_loop_thread(void)
{
	if (!pthread_equal(pthread_self(), main_thread))
		n_handler_wrong_thread++;
}

static Job *
new_job(int kind, const char *name, int count, Example__HelloReply_Closure cl,
	void *d)
{
	Job *j = calloc(1, sizeof(*j));

	j->kind = kind;
	snprintf(j->name, sizeof j->name, "%s", name ? name : "");
	j->count = count;
	j->cl = cl;
	j->d = d;
	return j;
}

static void
greeter_say_hello(Example__Greeter_Service *s, const Example__HelloRequest *req,
		  Example__HelloReply_Closure cl, void *d)
{
	(void) s;
	check_loop_thread();
	if (strcmp(req->name, "late") == 0) {
		/* parked: answered from a worker long after the connection and
		 * the server are gone */
		late_job = new_job(0, req->name, 0, cl, d);
		return;
	}
	enqueue(new_job(0, req->name, 0, cl, d));
}

static void
greeter_say_hello_many(Example__Greeter_Service *s,
		       const Example__HelloRequest *req,
		       Example__HelloReply_Closure cl, void *d)
{
	(void) s;
	check_loop_thread();
	enqueue(new_job(1, req->name, req->count, cl, d));
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

	check_loop_thread();
	if (m != NULL) {
		ss->total += ((const Example__HelloRequest *) m)->count;
	} else {
		enqueue(new_job(2, "", ss->total, ss->cl, ss->d));
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

/* ---- the client ---- */

static TestLoop *loop;
static ProtobufCGrpc2Server *server;
static ProtobufCGrpc2Client *client;
static ProtobufCService *svc;
static ProtobufCGrpc2IoSystem *timer_io;
static void *finish_timer, *watchdog, *pump_timer;
static int issued, outstanding;
#define MAX_OUTSTANDING 45	/* calls past the stream limit fail */
static int n_done, n_failed, n_bad, n_hello, n_many_end, n_many_msgs, n_sum;
static const int total_calls = N_ROUNDS * 3;

static void
finish_cb(void *unused)
{
	(void) unused;
	finished = 1;
	protobuf_c_grpc2_client_free(client);
	client = NULL;
	protobuf_c_grpc2_server_free(server);
	server = NULL;
	timer_io->timer_free(timer_io, watchdog);
	timer_io->timer_free(timer_io, pump_timer);
	timer_io->timer_free(timer_io, finish_timer);
	timer_io->destroy(timer_io);
}

static void on_hello(const Example__HelloReply *r, void *d);
static void on_many(const Example__HelloReply *r, void *d);
static void on_sum(const Example__HelloReply *r, void *d);

static void
pump_cb(void *unused)
{
	(void) unused;
	while (issued < N_ROUNDS && outstanding + 3 <= MAX_OUTSTANDING) {
		Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
		Example__HelloRequest__InputStream *in;
		char name[32];
		int k;

		snprintf(name, sizeof name, "c%d", issued++);
		req.name = name;
		example__greeter__say_hello(svc, &req, on_hello, NULL);
		req.count = 3;
		example__greeter__say_hello_many(svc, &req, on_many, NULL);
		in = example__greeter__collect(svc, NULL, on_sum, NULL);
		assert(in != NULL);
		for (k = 1; k <= 4; k++) {
			req.count = k;
			example__hello_request__input_stream__send(in, &req);
		}
		example__hello_request__input_stream__send(in, NULL);
		outstanding += 3;
	}
}

static ProtobufCGrpc2IoSystem *keepalive_io;
static void *keepalive;

static void
keepalive_cb(void *unused)
{
	(void) unused;
	keepalive_io->timer_free(keepalive_io, keepalive);
	keepalive_io->destroy(keepalive_io);
}

static void
watchdog_cb(void *unused)
{
	(void) unused;
	fprintf(stderr, "threads-server: timed out (%d of %d calls done)\n",
		n_done, total_calls);
	abort();
}

static void
on_done(const ProtobufCMethodDescriptor *m, int status, const char *msg, void *ud)
{
	(void) m; (void) msg; (void) ud;
	if (finished)		/* the parked call, ended by the client's free */
		return;
	outstanding--;
	if (issued < N_ROUNDS)
		timer_io->timer_start(timer_io, pump_timer, 0);
	if (status != PROTOBUF_C_GRPC2_OK) {
		if (n_failed++ < 3)
			fprintf(stderr, "failed: %s %s\n",
				protobuf_c_grpc2_status_name(status), msg ? msg : "");
	}
	if (++n_done == total_calls)
		timer_io->timer_start(timer_io, finish_timer, 0);
}

static void
on_hello(const Example__HelloReply *r, void *d)
{
	(void) d;
	if (r == NULL)
		return;
	n_hello++;
	if (strncmp(r->message, "Hello, c", 8) != 0)
		n_bad++;
}

static void
on_many(const Example__HelloReply *r, void *d)
{
	(void) d;
	if (r == NULL)
		n_many_end++;
	else if (r->message[0] == '#')
		n_many_msgs++;
	else
		n_bad++;
}

static void
on_sum(const Example__HelloReply *r, void *d)
{
	(void) d;
	if (r == NULL)
		return;
	n_sum++;
	if (strcmp(r->message, "total=10") != 0)
		n_bad++;
}

int
main(void)
{
	ProtobufCService *services[] = { NULL };
	Example__Greeter_Service greeter =
		EXAMPLE__GREETER__INIT(greeter_);
	ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
	ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
	pthread_t th[N_WORKERS];
	unsigned port = test_free_port();
	int i;

	main_thread = pthread_self();
	services[0] = &greeter.base;
	loop = test_loop_new();
	assert(loop != NULL);

	/* a system that cannot post cannot serve a thread safe server */
	{
		ProtobufCGrpc2ServerOptions bad = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
		ProtobufCGrpc2IoSystem *real = test_loop_new_io(loop);
		ProtobufCGrpc2IoSystem fake = *real;

		fake.post = NULL;
		bad.services = services;
		bad.n_services = 1;
		bad.io = &fake;
		bad.thread_safe = 1;
		assert(protobuf_c_grpc2_server_new(&bad) == NULL);
		real->ref(real);
		real->destroy(real);
	}

	so.services = services;
	so.n_services = 1;
	so.io = test_loop_new_io(loop);
	so.thread_safe = 1;
	server = protobuf_c_grpc2_server_new(&so);
	assert(server != NULL);
	assert(protobuf_c_grpc2_server_listen(server, "127.0.0.1", port) == 0);

	co.host = "127.0.0.1";
	co.port = port;
	co.io = test_loop_new_io(loop);
	co.on_call_done = on_done;
	client = protobuf_c_grpc2_client_new(&co);
	assert(client != NULL);
	svc = protobuf_c_grpc2_client_get_service(client,
						  &example__greeter__descriptor);

	timer_io = test_loop_new_io(loop);
	timer_io->ref(timer_io);
	finish_timer = timer_io->timer_new(timer_io, finish_cb, NULL);
	watchdog = timer_io->timer_new(timer_io, watchdog_cb, NULL);
	pump_timer = timer_io->timer_new(timer_io, pump_cb, NULL);
	timer_io->timer_start(timer_io, watchdog, 30000);

	for (i = 0; i < N_WORKERS; i++)
		assert(pthread_create(&th[i], NULL, worker, NULL) == 0);

	timer_io->timer_start(timer_io, pump_timer, 0);
	{
		Example__HelloRequest late = EXAMPLE__HELLO_REQUEST__INIT;

		late.name = "late";
		example__greeter__say_hello(svc, &late, on_hello, NULL);
	}
	test_loop_run(loop);
	assert(late_job != NULL);	/* the handler holds its closure */
	enqueue(late_job);		/* answered by a worker, after the server is freed */

	pthread_mutex_lock(&qmu);
	stopping = 1;
	pthread_cond_broadcast(&qcv);
	pthread_mutex_unlock(&qmu);
	for (i = 0; i < N_WORKERS; i++)
		assert(pthread_join(th[i], NULL) == 0);

	/* the late response is posted to a loop that has finished: running the
	 * loop again delivers it, which releases the call and the I/O system */
	{
		ProtobufCGrpc2IoSystem *io = test_loop_new_io(loop);

		io->ref(io);
		keepalive_io = io;
		keepalive = io->timer_new(io, keepalive_cb, NULL);
		io->timer_start(io, keepalive, 100);
		test_loop_run(loop);
	}

	assert(n_done == total_calls);
	assert(n_failed == 0 && n_bad == 0 && n_handler_wrong_thread == 0);
	assert(n_hello == N_ROUNDS && n_many_end == N_ROUNDS &&
	       n_many_msgs == 3 * N_ROUNDS && n_sum == N_ROUNDS);
	assert(test_loop_close(loop) == 0);
	printf("threads-server (%s): ok\n", test_backend_name());
	return 0;
}
