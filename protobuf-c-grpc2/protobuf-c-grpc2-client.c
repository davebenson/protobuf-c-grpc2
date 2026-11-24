/*
 * Client builder and the client "channel": lazy connect, queued calls,
 * reconnect and backoff on top of the pluggable I/O and TLS interfaces.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2-internal.h"

#define DEFAULT_BACKOFF_INITIAL_MS 1000u
#define DEFAULT_BACKOFF_MAX_MS 120000u
#define DEFAULT_CONNECT_TIMEOUT_MS 20000u

/* ------------------------------------------------------------------ */
/* builder */

struct ProtobufCGrpc2ClientBuilder {
	char *host;
	unsigned port;
	char *server_name;
	char *authority;
	char *scheme;
	ProtobufCGrpc2ConnOptions options;
	uint64_t backoff_initial_ms, backoff_max_ms, connect_timeout_ms;
	ProtobufCGrpc2IoSystem *io;
	ProtobufCGrpc2SecureTransportSystem *sts;
};

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

ProtobufCGrpc2ClientBuilder *
protobuf_c_grpc2_client_builder_new(void)
{
	ProtobufCGrpc2ClientBuilder *b = calloc(1, sizeof(*b));

	if (b != NULL) {
		b->backoff_initial_ms = DEFAULT_BACKOFF_INITIAL_MS;
		b->backoff_max_ms = DEFAULT_BACKOFF_MAX_MS;
		b->connect_timeout_ms = DEFAULT_CONNECT_TIMEOUT_MS;
	}
	return b;
}

void
protobuf_c_grpc2_client_builder_free(ProtobufCGrpc2ClientBuilder *b)
{
	if (b == NULL)
		return;
	if (b->io != NULL && b->io->destroy != NULL)
		b->io->destroy(b->io);
	if (b->sts != NULL && b->sts->destroy != NULL)
		b->sts->destroy(b->sts);
	free(b->host);
	free(b->server_name);
	free(b->authority);
	free(b->scheme);
	free(b);
}

int
protobuf_c_grpc2_client_builder_set_target(ProtobufCGrpc2ClientBuilder *b,
					   const char *host, unsigned port)
{
	char *h = dup_or_null(host);

	if (h == NULL)
		return -1;
	free(b->host);
	b->host = h;
	b->port = port;
	return 0;
}

int
protobuf_c_grpc2_client_builder_set_server_name(ProtobufCGrpc2ClientBuilder *b,
						const char *server_name)
{
	char *n = dup_or_null(server_name);

	if (n == NULL)
		return -1;
	free(b->server_name);
	b->server_name = n;
	return 0;
}

int
protobuf_c_grpc2_client_builder_set_options(ProtobufCGrpc2ClientBuilder *b,
					    const ProtobufCGrpc2ConnOptions *o)
{
	char *auth = dup_or_null(o->authority);
	char *scheme = dup_or_null(o->scheme);

	if ((o->authority != NULL && auth == NULL) ||
	    (o->scheme != NULL && scheme == NULL)) {
		free(auth);
		free(scheme);
		return -1;
	}
	free(b->authority);
	free(b->scheme);
	b->authority = auth;
	b->scheme = scheme;
	b->options = *o;
	b->options.authority = NULL;	/* owned by the builder, set later */
	b->options.scheme = NULL;
	return 0;
}

void
protobuf_c_grpc2_client_builder_set_backoff(ProtobufCGrpc2ClientBuilder *b,
					    uint64_t initial_ms, uint64_t max_ms)
{
	b->backoff_initial_ms = initial_ms;
	b->backoff_max_ms = max_ms < initial_ms ? initial_ms : max_ms;
}

void
protobuf_c_grpc2_client_builder_set_connect_timeout_ms(
	ProtobufCGrpc2ClientBuilder *b, uint64_t ms)
{
	b->connect_timeout_ms = ms;
}

int
protobuf_c_grpc2_client_builder_add_io_system(ProtobufCGrpc2ClientBuilder *b,
					      ProtobufCGrpc2IoSystem *io)
{
	if (b->io != NULL)
		return -1;
	b->io = io;
	return 0;
}

int
protobuf_c_grpc2_client_builder_add_secure_transport_system(
	ProtobufCGrpc2ClientBuilder *b, ProtobufCGrpc2SecureTransportSystem *sts)
{
	if (b->sts != NULL)
		return -1;
	b->sts = sts;
	return 0;
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
};

const ProtobufCGrpc2ConnOptions *
protobuf_c_grpc2__client_options(const ProtobufCGrpc2Client *client)
{
	return &client->options;
}

ProtobufCGrpc2Client *
protobuf_c_grpc2_client_from_builder(ProtobufCGrpc2ClientBuilder *b)
{
	ProtobufCGrpc2Client *c;

	if (b->io == NULL || b->host == NULL)
		return NULL;
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->host = b->host;
	c->port = b->port;
	c->server_name = b->server_name != NULL ? b->server_name : dup_or_null(b->host);
	c->scheme = b->scheme != NULL ? b->scheme
		: dup_or_null(b->sts != NULL ? "https" : "http");
	if (b->authority != NULL) {
		c->authority = b->authority;
	} else {
		char text[300];
		snprintf(text, sizeof text, "%s:%u", b->host, b->port);
		c->authority = dup_or_null(text);
	}
	b->host = b->server_name = b->authority = b->scheme = NULL; /* moved */
	c->options = b->options;
	c->options.authority = c->authority;
	c->options.scheme = c->scheme;
	c->backoff_initial_ms = b->backoff_initial_ms;
	c->backoff_max_ms = b->backoff_max_ms;
	c->connect_timeout_ms = b->connect_timeout_ms;
	c->io = b->io;
	c->sts = b->sts;
	b->io = NULL;
	b->sts = NULL;
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
