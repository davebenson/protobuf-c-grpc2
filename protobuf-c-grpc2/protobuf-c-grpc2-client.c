/*
 * The client "channel": creation, lazy connect, queued calls,
 * reconnect and backoff on top of the pluggable I/O and TLS interfaces.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2-internal.h"

#define DEFAULT_BACKOFF_INITIAL_MS 1000u
#define DEFAULT_BACKOFF_MAX_MS 120000u
#define DEFAULT_CONNECT_TIMEOUT_MS 20000u

/* ------------------------------------------------------------------ */
/* creation */

static char *
dup_or_null(const char *s)
{
	size_t n;
	char *r;

	if (s == NULL)
		return NULL;
	n = strlen(s) + 1;
	r = malloc(n);
	if (r != NULL)
		memcpy(r, s, n);
	return r;
}

/* ------------------------------------------------------------------ */
/* the client */

enum { C_IDLE, C_CONNECTING, C_READY, C_BACKOFF };

struct ProtobufCGrpc2Client {
	ProtobufCGrpc2IoSystem *io;
	ProtobufCGrpc2SecureTransportSystem *sts;
	char *host;
	unsigned port;
	char *server_name;
	char *authority;
	char *scheme;
	ProtobufCGrpc2ConnOptions options;	/* authority/scheme filled in */
	uint64_t backoff_initial_ms, backoff_max_ms, connect_timeout_ms;
	uint64_t backoff_ms;			/* the current backoff delay */

	int state;
	ProtobufCGrpc2Conn *conn;		/* while CONNECTING or READY */
	ConnPump *pump;
	void *connect_handle;
	void *connect_timer, *backoff_timer, *hook;
	size_t pending_closes;			/* pumps still closing */

	ProtobufCService **handles;
	size_t n_handles;
	int shutting_down;

	/* thread_safe clients: calls from other threads are posted to the
	 * loop. `mu` guards the two fields below, the only state that other
	 * threads touch. */
	int thread_safe;
	pthread_t home;			/* the thread that created the client */
	pthread_mutex_t mu;
	size_t pending_posts;		/* posted, not yet run */
	int accepting;			/* 0 once client_free() has begun */
};

const ProtobufCGrpc2ConnOptions *
protobuf_c_grpc2__client_options(const ProtobufCGrpc2Client *client)
{
	return &client->options;
}

ProtobufCGrpc2Client *
protobuf_c_grpc2_client_new(const ProtobufCGrpc2ClientOptions *options)
{
	ProtobufCGrpc2ClientOptions o;
	ProtobufCGrpc2Client *c;
	char text[300];

	if (options == NULL ||
	    PROTOBUF_C_GRPC2__COPY_OPTIONS(&o, options) != 0)
		return NULL;
	if (o.io == NULL || o.host == NULL)
		return NULL;
	if (o.thread_safe && o.io->post == NULL)
		return NULL;
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	if (o.thread_safe && pthread_mutex_init(&c->mu, NULL) != 0) {
		free(c);
		return NULL;
	}
	c->thread_safe = o.thread_safe != 0;
	c->home = pthread_self();
	c->accepting = 1;
	c->host = dup_or_null(o.host);
	c->port = o.port;
	c->server_name = dup_or_null(o.server_name != NULL ? o.server_name : o.host);
	c->scheme = dup_or_null(o.secure_transport != NULL ? "https" : "http");
	if (o.authority != NULL) {
		c->authority = dup_or_null(o.authority);
	} else {
		snprintf(text, sizeof text, "%s:%u", o.host, o.port);
		c->authority = dup_or_null(text);
	}
	if (c->host == NULL || c->server_name == NULL || c->scheme == NULL ||
	    c->authority == NULL) {
		/* io and secure_transport stay with the caller */
		free(c->host);
		free(c->server_name);
		free(c->scheme);
		free(c->authority);
		if (c->thread_safe)
			pthread_mutex_destroy(&c->mu);
		free(c);
		return NULL;
	}
	c->options.struct_size = sizeof c->options;
	c->options.max_message_size = o.max_message_size;
	c->options.default_timeout_ms = o.default_timeout_ms;
	c->options.on_call_done = o.on_call_done;
	c->options.user_data = o.user_data;
	c->options.authority = c->authority;
	c->options.scheme = c->scheme;
	c->backoff_initial_ms = o.initial_backoff_ms ? o.initial_backoff_ms
		: DEFAULT_BACKOFF_INITIAL_MS;
	c->backoff_max_ms = o.max_backoff_ms ? o.max_backoff_ms
		: DEFAULT_BACKOFF_MAX_MS;
	if (c->backoff_max_ms < c->backoff_initial_ms)
		c->backoff_max_ms = c->backoff_initial_ms;
	c->connect_timeout_ms = o.connect_timeout_ms ? o.connect_timeout_ms
		: DEFAULT_CONNECT_TIMEOUT_MS;
	c->io = o.io;
	if (c->io->ref != NULL)
		c->io->ref(c->io);	/* else ownership is transferred */
	c->sts = o.secure_transport;
	if (c->sts != NULL && c->sts->ref != NULL)
		c->sts->ref(c->sts);	/* else ownership is transferred */
	c->state = C_IDLE;
	return c;
}

/* timers and the flush hook are created when first needed */

static void client_connect_timeout(void *arg);
static void client_backoff_done(void *arg);
static void client_hook(void *arg);

static int
client_setup(ProtobufCGrpc2Client *c)
{
	if (c->hook != NULL)
		return 0;
	c->connect_timer = c->io->timer_new(c->io, client_connect_timeout, c);
	c->backoff_timer = c->io->timer_new(c->io, client_backoff_done, c);
	c->hook = c->io->add_iteration_hook(c->io, client_hook, c);
	return (c->connect_timer && c->backoff_timer && c->hook) ? 0 : -1;
}

static void
client_hook(void *arg)
{
	ProtobufCGrpc2Client *c = arg;
	if (c->pump != NULL)
		protobuf_c_grpc2__pump_flush(c->pump);
}

static void
client_finalize_if_done(ProtobufCGrpc2Client *c)
{
	size_t i;

	if (!c->shutting_down || c->pending_closes > 0 || c->connect_handle != NULL)
		return;
	if (c->thread_safe) {
		size_t pending;

		pthread_mutex_lock(&c->mu);
		pending = c->pending_posts;
		pthread_mutex_unlock(&c->mu);
		if (pending > 0)
			return;		/* the last posted function finalizes */
		pthread_mutex_destroy(&c->mu);
	}
	if (c->hook != NULL)
		c->io->remove_iteration_hook(c->io, c->hook);
	if (c->connect_timer != NULL)
		c->io->timer_free(c->io, c->connect_timer);
	if (c->backoff_timer != NULL)
		c->io->timer_free(c->io, c->backoff_timer);
	if (c->io->destroy != NULL)
		c->io->destroy(c->io);
	if (c->sts != NULL && c->sts->destroy != NULL)
		c->sts->destroy(c->sts);
	for (i = 0; i < c->n_handles; i++)
		protobuf_c_grpc2__client_service_free(c->handles[i]);
	free(c->handles);
	free(c->host);
	free(c->server_name);
	free(c->authority);
	free(c->scheme);
	free(c);
}

/* The next backoff delay: initial, then x1.6 up to the maximum. */
static uint64_t
client_next_backoff(ProtobufCGrpc2Client *c)
{
	if (c->backoff_ms == 0)
		c->backoff_ms = c->backoff_initial_ms;
	else
		c->backoff_ms = c->backoff_ms * 16 / 10;
	if (c->backoff_ms > c->backoff_max_ms)
		c->backoff_ms = c->backoff_max_ms;
	if (c->backoff_ms == 0)
		c->backoff_ms = 1;
	return c->backoff_ms;
}

static void
client_enter_backoff(ProtobufCGrpc2Client *c)
{
	c->state = C_BACKOFF;
	if (c->backoff_timer != NULL)
		c->io->timer_start(c->io, c->backoff_timer, client_next_backoff(c));
}

static void
client_backoff_done(void *arg)
{
	ProtobufCGrpc2Client *c = arg;
	if (c->state == C_BACKOFF)
		c->state = C_IDLE;
}

/* A connection attempt failed before a pump existed. */
static void
client_attempt_failed(ProtobufCGrpc2Client *c)
{
	ProtobufCGrpc2Conn *conn = c->conn;

	c->conn = NULL;
	if (!c->shutting_down)
		client_enter_backoff(c);
	else
		c->state = C_IDLE;
	/* completes the queued calls with UNAVAILABLE */
	if (conn != NULL)
		protobuf_c_grpc2_conn_free(conn);
}

static void
client_connect_timeout(void *arg)
{
	ProtobufCGrpc2Client *c = arg;

	if (c->state != C_CONNECTING || c->connect_handle == NULL)
		return;
	c->io->cancel_connect(c->io, c->connect_handle);
	c->connect_handle = NULL;
	client_attempt_failed(c);
	client_finalize_if_done(c);
}

static void
client_pump_closing(ConnPump *p, void *arg)
{
	ProtobufCGrpc2Client *c = arg;

	/* Stop using the connection before it is freed (freeing completes the
	 * calls in flight, whose callbacks may start new calls). */
	c->pump = NULL;
	c->conn = NULL;
	if (c->shutting_down) {
		c->state = C_IDLE;
	} else if (p->established) {
		c->state = C_IDLE;	/* a working connection was lost */
		c->backoff_ms = 0;
	} else {
		client_enter_backoff(c);	/* never got an answer */
	}
}

static void
client_pump_closed(void *arg, int established)
{
	ProtobufCGrpc2Client *c = arg;
	(void) established;
	c->pending_closes--;
	client_finalize_if_done(c);
}

static void
client_connected(void *arg, void *stream, int error)
{
	ProtobufCGrpc2Client *c = arg;
	ProtobufCGrpc2SecureTransport *tls = NULL;
	ConnPump *p;

	c->connect_handle = NULL;
	if (c->connect_timer != NULL)
		c->io->timer_stop(c->io, c->connect_timer);
	if (c->shutting_down) {
		if (stream != NULL)
			c->io->stream_close(c->io, stream);
		client_attempt_failed(c);
		client_finalize_if_done(c);
		return;
	}
	if (error != 0 || stream == NULL) {
		client_attempt_failed(c);
		return;
	}
	if (c->sts != NULL) {
		tls = c->sts->new_client(c->sts, c->server_name);
		if (tls == NULL) {
			c->io->stream_close(c->io, stream);
			client_attempt_failed(c);
			return;
		}
	}
	p = protobuf_c_grpc2__pump_new(c->io, stream, c->conn, tls,
				       client_pump_closing, client_pump_closed, c);
	if (p == NULL) {
		if (tls != NULL)
			tls->vtable->destroy(tls);
		client_attempt_failed(c);
		return;
	}
	c->pump = p;
	c->pending_closes++;
	c->state = C_READY;
}

static int
client_start_connect(ProtobufCGrpc2Client *c)
{
	ProtobufCGrpc2Conn *conn;

	if (client_setup(c) != 0)
		return -1;
	conn = protobuf_c_grpc2_conn_client_new(&c->options);
	if (conn == NULL)
		return -1;
	if (c->io->connect(c->io, c->host, c->port, client_connected, c,
			   &c->connect_handle) != 0) {
		protobuf_c_grpc2_conn_free(conn);
		return -1;
	}
	c->conn = conn;
	c->state = C_CONNECTING;
	if (c->connect_timeout_ms > 0)
		c->io->timer_start(c->io, c->connect_timer, c->connect_timeout_ms);
	return 0;
}

ProtobufCGrpc2Conn *
protobuf_c_grpc2__client_acquire_conn(ProtobufCGrpc2Client *c)
{
	if (c->shutting_down)
		return NULL;
	switch (c->state) {
	case C_CONNECTING:
	case C_READY:
		return c->conn;
	case C_BACKOFF:
		return NULL;
	default:
		break;
	}
	if (client_start_connect(c) != 0) {
		client_enter_backoff(c);
		return NULL;
	}
	return c->conn;
}

ProtobufCService *
protobuf_c_grpc2_client_get_service(ProtobufCGrpc2Client *c,
				    const ProtobufCServiceDescriptor *descriptor)
{
	ProtobufCService *svc;
	ProtobufCService **h;

	if (c->shutting_down)
		return NULL;
	svc = protobuf_c_grpc2__client_service_new(c, descriptor,
						   c->options.default_timeout_ms);
	if (svc == NULL)
		return NULL;
	h = realloc(c->handles, (c->n_handles + 1) * sizeof(*h));
	if (h == NULL) {
		protobuf_c_grpc2__client_service_free(svc);
		return NULL;
	}
	c->handles = h;
	h[c->n_handles++] = svc;
	return svc;
}

void
protobuf_c_grpc2_client_free(ProtobufCGrpc2Client *c)
{
	if (c == NULL || c->shutting_down)
		return;
	if (c->thread_safe) {
		pthread_mutex_lock(&c->mu);
		c->accepting = 0;
		pthread_mutex_unlock(&c->mu);
	}
	c->shutting_down = 1;
	if (c->connect_handle != NULL) {
		c->io->cancel_connect(c->io, c->connect_handle);
		c->connect_handle = NULL;
	}
	if (c->connect_timer != NULL)
		c->io->timer_stop(c->io, c->connect_timer);
	if (c->backoff_timer != NULL)
		c->io->timer_stop(c->io, c->backoff_timer);
	if (c->pump != NULL)
		protobuf_c_grpc2__pump_close(c->pump);
	else if (c->conn != NULL)
		client_attempt_failed(c);	/* queued calls, never connected */
	client_finalize_if_done(c);
}

/* ------------------------------------------------------------------ */
/* calls from other threads */

int
protobuf_c_grpc2__client_is_foreign(const ProtobufCGrpc2Client *c)
{
	return c->thread_safe && !pthread_equal(pthread_self(), c->home);
}

/* The request side of a call started from another thread: what is sent on it
 * is queued behind the call itself, and the loop passes it to the real stream
 * (which only the loop thread touches). */
typedef struct {
	ProtobufCInputStream base;	/* must be first */
	ProtobufCGrpc2Client *client;
	ProtobufCInputStream *real;	/* NULL if the call did not start */
} ForeignSink;

enum { OP_CALL, OP_SEND, OP_END };

typedef struct {
	ProtobufCGrpc2Client *client;
	int kind;
	ProtobufCService *service;	/* OP_CALL */
	unsigned index_and_flags;
	uint64_t timeout_ms;
	ProtobufCClosure closure;
	void *closure_data;
	ForeignSink *sink;		/* OP_SEND, OP_END; OP_CALL if streaming */
	const ProtobufCMessageDescriptor *desc;	/* of the packed message */
	uint8_t *data;
	size_t len;
	int has_message;
} Op;

static void
op_run(void *arg)
{
	Op *op = arg;
	ProtobufCGrpc2Client *c = op->client;
	ProtobufCMessage *m = NULL;

	if (op->has_message) {
		m = protobuf_c_message_unpack(op->desc, NULL, op->len, op->data);
		if (m == NULL && op->kind == OP_CALL && op->closure != NULL)
			op->closure(NULL, op->closure_data);
	}
	switch (op->kind) {
	case OP_CALL:
		if (!op->has_message || m != NULL) {
			ProtobufCInputStream *real =
				protobuf_c_grpc2__client_invoke_local(
					op->service, op->timeout_ms,
					op->index_and_flags, m, op->closure,
					op->closure_data);
			if (op->sink != NULL)
				op->sink->real = real;
			else if (real != NULL)
				real->send(real, NULL);	/* cannot happen */
		}
		break;
	case OP_SEND:
		if (op->sink->real != NULL && m != NULL)
			op->sink->real->send(op->sink->real, m);
		break;
	case OP_END:
		if (op->sink->real != NULL)
			op->sink->real->send(op->sink->real, NULL);
		free(op->sink);
		break;
	}
	if (m != NULL)
		protobuf_c_message_free_unpacked(m, NULL);
	free(op->data);
	free(op);

	/* the client may only go once nothing posted is left to run */
	pthread_mutex_lock(&c->mu);
	c->pending_posts--;
	pthread_mutex_unlock(&c->mu);
	client_finalize_if_done(c);
}

/* Takes `op` on success; on failure the caller still owns it. */
static int
op_post(ProtobufCGrpc2Client *c, Op *op)
{
	pthread_mutex_lock(&c->mu);
	if (!c->accepting) {
		pthread_mutex_unlock(&c->mu);
		return -1;
	}
	c->pending_posts++;
	pthread_mutex_unlock(&c->mu);
	if (c->io->post(c->io, op_run, op) != 0) {
		pthread_mutex_lock(&c->mu);
		c->pending_posts--;
		pthread_mutex_unlock(&c->mu);
		return -1;
	}
	return 0;
}

static Op *
op_new(ProtobufCGrpc2Client *c, int kind, const ProtobufCMessageDescriptor *desc,
       const ProtobufCMessage *msg)
{
	Op *op = calloc(1, sizeof(*op));

	if (op == NULL)
		return NULL;
	op->client = c;
	op->kind = kind;
	if (msg != NULL) {
		size_t n = protobuf_c_message_get_packed_size(msg);

		op->data = malloc(n ? n : 1);
		if (op->data == NULL) {
			free(op);
			return NULL;
		}
		protobuf_c_message_pack(msg, op->data);
		op->len = n;
		op->desc = desc;
		op->has_message = 1;
	}
	return op;
}

static protobuf_c_boolean
foreign_sink_send(ProtobufCInputStream *s, const ProtobufCMessage *msg)
{
	ForeignSink *fs = (ForeignSink *) s;
	Op *op = op_new(fs->client, msg != NULL ? OP_SEND : OP_END,
			s->input_descriptor, msg);

	if (op == NULL)
		return 0;
	op->sink = fs;
	if (op_post(fs->client, op) != 0) {
		free(op->data);
		free(op);
		return 0;
	}
	return 1;	/* the end of the stream frees `fs` when it has run */
}

ProtobufCInputStream *
protobuf_c_grpc2__client_invoke_foreign(ProtobufCGrpc2Client *c,
					ProtobufCService *service,
					unsigned index_and_flags,
					uint64_t timeout_ms,
					const ProtobufCMessage *input,
					ProtobufCClosure closure,
					void *closure_data)
{
	unsigned index = index_and_flags & ~PROTOBUF_C_STREAMING_MASK;
	const ProtobufCMethodDescriptor *m = &service->descriptor->methods[index];
	ForeignSink *sink = NULL;
	Op *op = op_new(c, OP_CALL, m->input, input);

	if (op == NULL)
		goto fail;
	op->service = service;
	op->index_and_flags = index_and_flags;
	op->timeout_ms = timeout_ms;
	op->closure = closure;
	op->closure_data = closure_data;
	if (index_and_flags & PROTOBUF_C_INPUT_STREAMING) {
		sink = calloc(1, sizeof(*sink));
		if (sink == NULL)
			goto fail;
		sink->base.send = foreign_sink_send;
		sink->base.input_descriptor = m->input;
		sink->client = c;
		op->sink = sink;
	}
	if (op_post(c, op) != 0)
		goto fail;
	return sink != NULL ? &sink->base : NULL;
fail:
	if (op != NULL) {
		free(op->data);
		free(op);
	}
	free(sink);
	/* the client is closing, or out of memory: reported here, on the
	 * calling thread, as the loop cannot be asked */
	if (closure != NULL)
		closure(NULL, closure_data);
	return NULL;
}
