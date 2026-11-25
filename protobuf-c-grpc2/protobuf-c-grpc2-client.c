/*
 * The client "channel": creation, lazy connect, queued calls,
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
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
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
