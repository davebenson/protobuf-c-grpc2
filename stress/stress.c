/*
 * stress: forks a server and N clients, each client keeping a fixed number of
 * requests in flight until it has had a given number of them answered.
 *
 *   usage: stress [-n clients] [-m method] [-p pending] [-r requests]
 *                 [-d duration_ms] [-c echo_messages] [-P port] [-t seconds]
 *
 *   -n  number of client processes                       (default 1)
 *   -m  noop | spin | delay | echo | mixed               (default noop)
 *         noop   the server answers at once
 *         spin   the server busy-waits for -d milliseconds, then answers
 *         delay  the server answers after -d milliseconds, from a timer
 *         echo   bidirectional stream: -c request messages, each answered
 *         mixed  rotate through all of the above
 *   -p  requests each client keeps in flight             (default 10)
 *   -r  responses each client waits for before it stops  (default 1000)
 *   -d  duration for spin and delay, in milliseconds     (default 1)
 *   -c  messages per echo call                           (default 4)
 *   -P  server port                                      (default 50099)
 *   -t  give up (and kill everything) after this long    (default 120 s)
 *
 * The exit status is nonzero if any call failed or anything went missing.
 * Linked with one event loop backend, see t/loop-backend.h.
 */
#include <assert.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "loop-backend.h"
#include "stress.pb-c.h"

typedef enum { M_NOOP, M_SPIN, M_DELAY, M_ECHO, M_MIXED } Method;
static const char *const method_names[] = {
	"noop", "spin", "delay", "echo", "mixed"
};

static struct {
	int n_clients;
	Method method;
	int pending;
	long requests;
	int duration_ms;
	int echo_count;
	unsigned port;
	int timeout_s;
} cfg = { 1, M_NOOP, 10, 1000, 1, 4, 50099, 120 };

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int64_t
now_us_gtod(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}

/* ------------------------------------------------------------------ */
/* the server */

static ProtobufCGrpc2IoSystem *server_io;

static void
stress_noop(Stress__Stress_Service *s, const Stress__Req *req,
	    Stress__Rsp_Closure closure, void *d)
{
	Stress__Rsp rsp = STRESS__RSP__INIT;
	(void) s;
	rsp.id = req->id;
	rsp.sent_ns = req->sent_ns;
	closure(&rsp, d);
}

static void
stress_spin(Stress__Stress_Service *s, const Stress__Req *req,
	    Stress__Rsp_Closure closure, void *d)
{
	int64_t end = now_us_gtod() + (int64_t) req->duration_ms * 1000;

	while (now_us_gtod() < end)
		;
	stress_noop(s, req, closure, d);
}

typedef struct {
	void *timer;
	int64_t id, sent_ns;
	Stress__Rsp_Closure closure;
	void *closure_data;
} DelayState;

static void
delay_fire(void *arg)
{
	DelayState *st = arg;
	Stress__Rsp rsp = STRESS__RSP__INIT;

	rsp.id = st->id;
	rsp.sent_ns = st->sent_ns;
	st->closure(&rsp, st->closure_data);
	server_io->timer_free(server_io, st->timer);
	free(st);
}

static void
stress_delay(Stress__Stress_Service *s, const Stress__Req *req,
	     Stress__Rsp_Closure closure, void *d)
{
	DelayState *st = calloc(1, sizeof(*st));
	(void) s;

	assert(st != NULL);
	st->id = req->id;
	st->sent_ns = req->sent_ns;
	st->closure = closure;
	st->closure_data = d;
	st->timer = server_io->timer_new(server_io, delay_fire, st);
	assert(st->timer != NULL);
	server_io->timer_start(server_io, st->timer, (uint64_t) req->duration_ms);
}

typedef struct {
	ProtobufCInputStream base;	/* must be first */
	Stress__Rsp_Closure closure;
	void *closure_data;
} EchoStream;

static protobuf_c_boolean
echo_send_input(ProtobufCInputStream *stream, const ProtobufCMessage *msg)
{
	EchoStream *es = (EchoStream *) stream;

	if (msg == NULL) {
		es->closure(NULL, es->closure_data);	/* end of responses */
	} else {
		const Stress__Req *req = (const Stress__Req *) msg;
		Stress__Rsp rsp = STRESS__RSP__INIT;

		rsp.id = req->id;
		rsp.sent_ns = req->sent_ns;
		es->closure(&rsp, es->closure_data);
	}
	return 1;
}

static void
echo_destroy(ProtobufCInputStream *stream)
{
	free(stream);
}

static const ProtobufCInputStreamVTable echo_vtable = {
	echo_send_input, echo_destroy
};

static ProtobufCInputStream *
stress_echo(Stress__Stress_Service *s, const Stress__Req *first,
	    Stress__Rsp_Closure closure, void *d)
{
	EchoStream *es = calloc(1, sizeof(*es));
	(void) s;

	assert(es != NULL);
	es->base.vtable = &echo_vtable;
	es->base.input_descriptor = &stress__req__descriptor;
	es->closure = closure;
	es->closure_data = d;
	if (first != NULL)
		echo_send_input(&es->base, &first->base);
	return &es->base;
}

/* Runs in the server process. Reports 'R' (listening) or 'F' (failed) on
 * `ready_fd`, then serves until it is killed. */
static void
run_server(int ready_fd)
{
	Stress__Stress_Service service = STRESS__STRESS__INIT(stress_);
	ProtobufCService *services[] = { &service.base };
	ProtobufCGrpc2ServerOptions opts = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
	ProtobufCGrpc2Server *server;
	TestLoop *loop = test_loop_new();
	char status;

	assert(loop != NULL);
	server_io = test_loop_new_io(loop);
	assert(server_io != NULL);
	opts.services = services;
	opts.n_services = 1;
	opts.io = server_io;
	server = protobuf_c_grpc2_server_new(&opts);	/* takes its own ref */
	if (server == NULL ||
	    protobuf_c_grpc2_server_listen(server, "127.0.0.1", cfg.port) != 0) {
		fprintf(stderr, "stress: cannot listen on port %u\n", cfg.port);
		status = 'F';
		if (write(ready_fd, &status, 1) < 0)
			;
		_exit(1);
	}
	status = 'R';
	if (write(ready_fd, &status, 1) < 0)
		_exit(1);
	close(ready_fd);
	test_loop_run(loop);	/* until killed */
	_exit(0);
}

/* ------------------------------------------------------------------ */
/* a client */

typedef struct {
	uint64_t completed, errors, replies;
	double elapsed_s;
	double mean_us;
	uint32_t min_us, p50_us, p99_us, max_us;
} Result;

static ProtobufCGrpc2Client *client;
static ProtobufCGrpc2IoSystem *client_io;
static void *finish_timer;
static ProtobufCService *svc;
static long issued, completed, pending, errors;
static uint64_t replies_expected, replies_got, wrong_replies;
static int filling;
static int64_t t_start, t_end;
static uint32_t *lat_us;
static size_t n_lat, cap_lat;

static void
record_latency(int64_t sent_ns)
{
	int64_t d = (now_ns() - sent_ns) / 1000;

	if (n_lat == cap_lat) {
		cap_lat = cap_lat ? cap_lat * 2 : 4096;
		lat_us = realloc(lat_us, cap_lat * sizeof(*lat_us));
		assert(lat_us != NULL);
	}
	lat_us[n_lat++] = d < 0 ? 0 : (uint32_t) d;
}

/* A response (or, with NULL, the end of a stream or a failure; the failure
 * itself is counted in on_call_done). The call's id travels as the closure
 * data, so that no per-call memory is needed. */
static void
on_rsp(const Stress__Rsp *rsp, void *closure_data)
{
	if (rsp == NULL)
		return;
	replies_got++;
	if (rsp->id != (int64_t) (intptr_t) closure_data)
		wrong_replies++;
	record_latency(rsp->sent_ns);
}

static void
start_one(void)
{
	Stress__Req req = STRESS__REQ__INIT;
	int64_t id = ++issued;
	Method m = cfg.method;
	void *tag = (void *) (intptr_t) id;

	if (m == M_MIXED)
		m = (Method) (id % 4);
	req.id = id;
	req.duration_ms = cfg.duration_ms;
	pending++;
	switch (m) {
	case M_NOOP:
	case M_SPIN:
	case M_DELAY:
		req.sent_ns = now_ns();
		replies_expected++;
		if (m == M_NOOP)
			stress__stress__noop(svc, &req, on_rsp, tag);
		else if (m == M_SPIN)
			stress__stress__spin(svc, &req, on_rsp, tag);
		else
			stress__stress__delay(svc, &req, on_rsp, tag);
		break;
	default: {
		Stress__Req__InputStream *in =
			stress__stress__echo(svc, NULL, on_rsp, tag);
		int i;

		if (in == NULL)
			break;
		for (i = 0; i < cfg.echo_count; i++) {
			req.sent_ns = now_ns();
			if (stress__req__input_stream__send(in, &req))
				replies_expected++;
		}
		stress__req__input_stream__end(in);
		stress__req__input_stream__destroy(in);
		break;
	}
	}
}

static void
fill(void)
{
	if (filling)
		return;		/* a call failed synchronously: the loop below goes on */
	filling = 1;
	while (pending < cfg.pending && issued < cfg.requests)
		start_one();
	filling = 0;
}

static void
finish(void *arg)
{
	(void) arg;
	/* Not from inside the client's own callback: from a timer. */
	protobuf_c_grpc2_client_free(client);
	client_io->timer_free(client_io, finish_timer);
	client_io->destroy(client_io);		/* our reference */
}

/* Called once for every finished call, whatever its type. */
static void
on_call_done(const ProtobufCMethodDescriptor *method, int status,
	     const char *message, void *user_data)
{
	(void) user_data;
	pending--;
	completed++;
	if (status != PROTOBUF_C_GRPC2_OK) {
		if (errors++ == 0)
			fprintf(stderr, "client %d: %s failed: %s%s%s\n",
				(int) getpid(), method ? method->name : "?",
				protobuf_c_grpc2_status_name(status),
				message ? " - " : "", message ? message : "");
	}
	if (completed == cfg.requests) {
		t_end = now_ns();
		client_io->timer_start(client_io, finish_timer, 0);
	} else {
		fill();
	}
}

static int
cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;
	return x < y ? -1 : x > y;
}

static void
summarize(Result *r)
{
	size_t i;
	double sum = 0;

	memset(r, 0, sizeof(*r));
	r->completed = (uint64_t) completed;
	r->errors = (uint64_t) errors;
	r->replies = replies_got;
	r->elapsed_s = (double) (t_end - t_start) / 1e9;
	if (n_lat == 0)
		return;
	qsort(lat_us, n_lat, sizeof(*lat_us), cmp_u32);
	for (i = 0; i < n_lat; i++)
		sum += lat_us[i];
	r->mean_us = sum / (double) n_lat;
	r->min_us = lat_us[0];
	r->p50_us = lat_us[n_lat / 2];
	r->p99_us = lat_us[(n_lat * 99) / 100 < n_lat ? (n_lat * 99) / 100 : n_lat - 1];
	r->max_us = lat_us[n_lat - 1];
}

/* Runs in a client process; returns its exit status. */
static int
run_client(int result_fd)
{
	TestLoop *loop = test_loop_new();
	ProtobufCGrpc2ClientOptions opts = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
	Result r;
	int bad;

	assert(loop != NULL);
	client_io = test_loop_new_io(loop);
	assert(client_io != NULL);
	opts.host = "127.0.0.1";
	opts.port = cfg.port;
	opts.io = client_io;
	opts.on_call_done = on_call_done;
	client = protobuf_c_grpc2_client_new(&opts);
	if (client == NULL) {
		fprintf(stderr, "stress: cannot create a client\n");
		return 1;
	}
	client_io->ref(client_io);		/* ours, for the finish timer */
	finish_timer = client_io->timer_new(client_io, finish, NULL);
	assert(finish_timer != NULL);
	svc = protobuf_c_grpc2_client_get_service(client,
						  &stress__stress__descriptor);
	assert(svc != NULL);

	t_start = now_ns();
	fill();
	test_loop_run(loop);

	bad = errors != 0 || wrong_replies != 0 || replies_got != replies_expected;
	if (wrong_replies != 0)
		fprintf(stderr, "client %d: %" PRIu64 " replies for the wrong call\n",
			(int) getpid(), wrong_replies);
	if (replies_got != replies_expected)
		fprintf(stderr, "client %d: %" PRIu64 " replies, expected %" PRIu64 "\n",
			(int) getpid(), replies_got, replies_expected);
	if (test_loop_close(loop) != 0) {
		fprintf(stderr, "client %d: leaked something on the loop\n",
			(int) getpid());
		bad = 1;
	}
	summarize(&r);
	if (write(result_fd, &r, sizeof r) != (ssize_t) sizeof r)
		bad = 1;
	return bad;
}

/* ------------------------------------------------------------------ */
/* the parent */

static pid_t *children;
static int n_children;

static void
on_alarm(int sig)
{
	int i;
	(void) sig;
	for (i = 0; i < n_children; i++)
		if (children[i] > 0)
			kill(children[i], SIGKILL);
	(void) !write(2, "stress: timed out\n", 18);
	_exit(2);
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-n clients] [-m noop|spin|delay|echo|mixed] [-p pending]\n"
		"          [-r requests] [-d duration_ms] [-c echo_messages]\n"
		"          [-P port] [-t seconds]\n", prog);
	exit(2);
}

static long
num_arg(const char *prog, const char *s, long min)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (*s == '\0' || *end != '\0' || v < min)
		usage(prog);
	return v;
}

int
main(int argc, char **argv)
{
	int opt, i, status, failed = 0;
	int ready[2], results[2];
	pid_t server_pid;
	char ready_byte = 0;
	Result total, r;
	int64_t t0, t1;
	double p50_sum = 0;
	uint32_t worst_p99 = 0, worst_max = 0;
	double mean_sum = 0;
	int n_results = 0;

	while ((opt = getopt(argc, argv, "n:m:p:r:d:c:P:t:h")) != -1) {
		switch (opt) {
		case 'n': cfg.n_clients = (int) num_arg(argv[0], optarg, 1); break;
		case 'p': cfg.pending = (int) num_arg(argv[0], optarg, 1); break;
		case 'r': cfg.requests = num_arg(argv[0], optarg, 1); break;
		case 'd': cfg.duration_ms = (int) num_arg(argv[0], optarg, 0); break;
		case 'c': cfg.echo_count = (int) num_arg(argv[0], optarg, 0); break;
		case 'P': cfg.port = (unsigned) num_arg(argv[0], optarg, 1); break;
		case 't': cfg.timeout_s = (int) num_arg(argv[0], optarg, 1); break;
		case 'm': {
			int k, found = 0;
			for (k = 0; k < 5; k++)
				if (strcmp(optarg, method_names[k]) == 0) {
					cfg.method = (Method) k;
					found = 1;
				}
			if (!found)
				usage(argv[0]);
			break;
		}
		default: usage(argv[0]);
		}
	}
	if (optind != argc)
		usage(argv[0]);

	children = calloc((size_t) cfg.n_clients + 1, sizeof(*children));
	assert(children != NULL);
	n_children = cfg.n_clients + 1;
	signal(SIGALRM, on_alarm);
	alarm((unsigned) cfg.timeout_s);
	if (pipe(ready) != 0 || pipe(results) != 0) {
		perror("pipe");
		return 1;
	}

	/* the server, then wait until it listens */
	server_pid = fork();
	if (server_pid < 0) {
		perror("fork");
		return 1;
	}
	if (server_pid == 0) {
		close(ready[0]);
		close(results[0]);
		close(results[1]);
		run_server(ready[1]);
	}
	children[0] = server_pid;
	close(ready[1]);
	if (read(ready[0], &ready_byte, 1) != 1 || ready_byte != 'R') {
		kill(server_pid, SIGKILL);
		waitpid(server_pid, NULL, 0);
		return 1;
	}
	close(ready[0]);

	t0 = now_ns();
	for (i = 0; i < cfg.n_clients; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork");
			failed = 1;
			break;
		}
		if (pid == 0) {
			close(results[0]);
			_exit(run_client(results[1]));
		}
		children[i + 1] = pid;
	}
	close(results[1]);

	memset(&total, 0, sizeof total);
	while (read(results[0], &r, sizeof r) == (ssize_t) sizeof r) {
		total.completed += r.completed;
		total.errors += r.errors;
		total.replies += r.replies;
		p50_sum += r.p50_us;
		mean_sum += r.mean_us;
		if (r.p99_us > worst_p99)
			worst_p99 = r.p99_us;
		if (r.max_us > worst_max)
			worst_max = r.max_us;
		n_results++;
	}
	for (i = 0; i < cfg.n_clients; i++) {
		if (children[i + 1] > 0 &&
		    waitpid(children[i + 1], &status, 0) > 0 &&
		    !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
			failed = 1;
	}
	t1 = now_ns();
	kill(server_pid, SIGTERM);
	waitpid(server_pid, NULL, 0);
	alarm(0);

	if (n_results != cfg.n_clients)
		failed = 1;
	printf("backend=%s method=%s clients=%d pending=%d requests/client=%ld "
	       "duration=%dms\n", test_backend_name(), method_names[cfg.method],
	       cfg.n_clients, cfg.pending, cfg.requests, cfg.duration_ms);
	printf("  %" PRIu64 " calls in %.2f s: %.0f calls/s, %" PRIu64 " replies, "
	       "%" PRIu64 " errors\n", total.completed, (double) (t1 - t0) / 1e9,
	       (double) total.completed / ((double) (t1 - t0) / 1e9),
	       total.replies, total.errors);
	if (n_results > 0)
		printf("  latency (us): mean %.0f, median %.0f, worst p99 %u, "
		       "max %u\n", mean_sum / n_results, p50_sum / n_results,
		       worst_p99, worst_max);
	puts(failed ? "FAILED" : "ok");
	return failed ? 1 : 0;
}
