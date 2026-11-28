/*
 * Servers that run their own connections: creation, the listeners and the
 * list of connections. The per-connection work (moving bytes between the I/O
 * stream, the optional TLS session and the HTTP/2 connection) is in pump.c.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2-internal.h"

#define IO_CHUNK 16384

/* ------------------------------------------------------------------ */
/* the authenticator */

void
protobuf_c_grpc2__server_release_auth(ProtobufCGrpc2Server *server)
{
	size_t i;

	for (i = 0; i < server->n_auth_names; i++)
		free(server->auth_names[i]);
	free(server->auth_names);
	server->auth_names = NULL;
	server->n_auth_names = 0;
	if (server->auth != NULL && server->auth->destroy != NULL)
		server->auth->destroy(server->auth);
	server->auth = NULL;
}

/* Copy the header names the authenticator wants, lower-cased. */
static int
server_setup_auth(ProtobufCGrpc2Server *server, ProtobufCGrpc2ServerAuth *auth)
{
	static const char *const dflt[] = { "authorization" };
	const char *const *names = auth->n_header_names > 0 ? auth->header_names
							     : dflt;
	size_t n = auth->n_header_names > 0 ? auth->n_header_names : 1;
	size_t i, j;

	if (auth->check == NULL)
		return -1;
	server->auth_names = calloc(n, sizeof(char *));
	if (server->auth_names == NULL)
		return -1;
	for (i = 0; i < n; i++) {
		char *s = names[i] != NULL ? strdup(names[i]) : NULL;

		if (s == NULL) {
			server->n_auth_names = i;
			return -1;
		}
		for (j = 0; s[j] != 0; j++)
			if (s[j] >= 'A' && s[j] <= 'Z')
				s[j] = (char) (s[j] - 'A' + 'a');
		server->auth_names[i] = s;
	}
	server->n_auth_names = n;
	return 0;
}

/* ------------------------------------------------------------------ */
/* responses from other threads (thread_safe servers) */

void
protobuf_c_grpc2__server_shared_unref(ServerShared *shared)
{
	if (--shared->refs > 0)
		return;
	if (shared->io->destroy != NULL)
		shared->io->destroy(shared->io);
	free(shared);
}

/* What a handler of a thread_safe server calls instead of the call's own
 * closure. One per call, alive until the final response has been given, and
 * holding a reference to `shared` so that it works even if the server is
 * freed first. (The call itself is kept alive by the reference the closure
 * holds, as for any closure.) */
typedef struct {
	ServerShared *shared;
	struct Call *call;
	const ProtobufCMessageDescriptor *output;	/* of the response */
	int output_streaming;
} SafeClosure;

typedef struct {
	SafeClosure *sc;
	uint8_t *data;			/* the packed response, if any */
	size_t len;
	int has_message;
} Response;

static void
safe_closure_free(SafeClosure *sc)
{
	protobuf_c_grpc2__server_shared_unref(sc->shared);
	free(sc);
}

static int
safe_closure_is_final(const SafeClosure *sc, const ProtobufCMessage *msg)
{
	return msg == NULL || !sc->output_streaming;
}

void *
protobuf_c_grpc2__safe_closure_new(ServerShared *shared, struct Call *call,
				   const ProtobufCMessageDescriptor *output,
				   int output_streaming)
{
	SafeClosure *sc = calloc(1, sizeof(*sc));

	if (sc == NULL)
		return NULL;
	sc->shared = shared;
	shared->refs++;
	sc->call = call;
	sc->output = output;
	sc->output_streaming = output_streaming;
	return sc;
}

static void
response_run(void *arg)
{
	Response *r = arg;
	SafeClosure *sc = r->sc;
	ProtobufCMessage *m = NULL;
	int final;

	if (r->has_message)
		m = protobuf_c_message_unpack(sc->output, NULL, r->len, r->data);
	final = safe_closure_is_final(sc, r->has_message ? (void *) r : NULL);
	/* a response that cannot be unpacked (it was packed from a valid
	 * message, so only out of memory) ends the call as a failure */
	protobuf_c_grpc2__server_respond(sc->call,
					 r->has_message && m == NULL ? NULL : m);
	if (m != NULL)
		protobuf_c_message_free_unpacked(m, NULL);
	free(r->data);
	free(r);
	if (final)
		safe_closure_free(sc);
}

void
protobuf_c_grpc2__safe_closure(const ProtobufCMessage *msg, void *data)
{
	SafeClosure *sc = data;
	Response *r;

	if (pthread_equal(pthread_self(), sc->shared->home)) {
		int final = safe_closure_is_final(sc, msg);

		protobuf_c_grpc2__server_respond(sc->call, msg);
		if (final)
			safe_closure_free(sc);
		return;
	}
	r = calloc(1, sizeof(*r));
	if (r != NULL && msg != NULL) {
		size_t n = protobuf_c_message_get_packed_size(msg);

		r->data = malloc(n ? n : 1);
		if (r->data != NULL) {
			protobuf_c_message_pack(msg, r->data);
			r->len = n;
			r->has_message = 1;
		}
		else {
			free(r);
			r = NULL;
		}
	}
	if (r != NULL)
		r->sc = sc;
	/* nowhere to report a failure to: the response would be lost and the
	 * call would never end */
	if (r == NULL || sc->shared->io->post(sc->shared->io, response_run, r) != 0)
		abort();
}

/* ------------------------------------------------------------------ */
/* connections (the per-connection work is in pump.c) */

static void
server_maybe_finish(ProtobufCGrpc2Server *server)
{
	if (!server->shutting_down || server->n_conns > 0)
		return;
	if (server->hook != NULL)
		server->io->remove_iteration_hook(server->io, server->hook);
	if (server->shared != NULL)
		protobuf_c_grpc2__server_shared_unref(server->shared);
	else if (server->io->destroy != NULL)
		server->io->destroy(server->io);
	if (server->sts != NULL && server->sts->destroy != NULL)
		server->sts->destroy(server->sts);
	protobuf_c_grpc2__server_release_auth(server);
	free(server->listeners);
	free(server->services);
	free(server);
}

static void
server_conn_closing(ConnPump *p, void *arg)
{
	ProtobufCGrpc2Server *server = arg;
	(void) server;
	*p->pprev = p->next;
	if (p->next != NULL)
		p->next->pprev = p->pprev;
}

static void
server_conn_closed(void *arg, int established)
{
	ProtobufCGrpc2Server *server = arg;
	(void) established;
	server->n_conns--;
	server_maybe_finish(server);
}

static void
server_iteration_hook(void *data)
{
	ProtobufCGrpc2Server *server = data;
	ConnPump *p;

	for (p = server->conns; p != NULL; p = p->next)
		protobuf_c_grpc2__pump_flush(p);
}

static void
server_accepted(void *data, void *stream)
{
	ProtobufCGrpc2Server *server = data;
	ProtobufCGrpc2IoSystem *io = server->io;
	ProtobufCGrpc2Conn *conn;
	ProtobufCGrpc2SecureTransport *tls = NULL;
	ConnPump *p;

	if (server->shutting_down) {
		io->stream_close(io, stream);
		return;
	}
	conn = protobuf_c_grpc2_conn_server_new(server, &server->conn_options);
	if (server->sts != NULL)
		tls = server->sts->new_server(server->sts);
	if (conn == NULL || (server->sts != NULL && tls == NULL)) {
		if (conn != NULL)
			protobuf_c_grpc2_conn_free(conn);
		if (tls != NULL)
			tls->vtable->destroy(tls);
		io->stream_close(io, stream);
		return;
	}
	p = protobuf_c_grpc2__pump_new(io, stream, conn, tls, server_conn_closing,
				       server_conn_closed, server);
	if (p == NULL) {
		protobuf_c_grpc2_conn_free(conn);
		if (tls != NULL)
			tls->vtable->destroy(tls);
		return;
	}
	p->next = server->conns;
	p->pprev = &server->conns;
	if (server->conns != NULL)
		server->conns->pprev = &p->next;
	server->conns = p;
	server->n_conns++;
}

/* ------------------------------------------------------------------ */
/* server */

ProtobufCGrpc2Server *
protobuf_c_grpc2_server_new(const ProtobufCGrpc2ServerOptions *options)
{
	ProtobufCGrpc2ServerOptions o;
	ProtobufCGrpc2Server *server;
	size_t i;

	memset(&o, 0, sizeof o);
	if (options != NULL &&
	    PROTOBUF_C_GRPC2__COPY_OPTIONS(&o, options) != 0)
		return NULL;
	if ((o.n_services > 0 && o.services == NULL) ||
	    (o.io == NULL && o.secure_transport != NULL))
		return NULL;
	if (o.thread_safe && (o.io == NULL || o.io->post == NULL))
		return NULL;
	server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;
	for (i = 0; i < o.n_services; i++) {
		if (protobuf_c_grpc2_server_add_service(server, o.services[i]) != 0) {
			free(server->services);
			free(server);
			return NULL;
		}
	}
	if (o.auth != NULL) {
		if (server_setup_auth(server, o.auth) != 0) {
			size_t k;

			for (k = 0; k < server->n_auth_names; k++)
				free(server->auth_names[k]);
			free(server->auth_names);
			free(server->services);
			free(server);
			return NULL;
		}
	}
	server->conn_options.struct_size = sizeof server->conn_options;
	server->conn_options.max_message_size = o.max_message_size;
	server->conn_options.default_timeout_ms = o.default_timeout_ms;
	server->conn_options.max_timeout_ms = o.max_timeout_ms;
	server->conn_options.on_call_done = o.on_call_done;
	server->conn_options.user_data = o.user_data;
	if (o.io != NULL) {
		server->hook = o.io->add_iteration_hook(o.io, server_iteration_hook,
							server);
		if (server->hook == NULL) {
			/* io and secure_transport stay with the caller */
			protobuf_c_grpc2__server_release_auth(server);
			free(server->services);
			free(server);
			return NULL;
		}
		if (o.thread_safe) {
			server->shared = calloc(1, sizeof(*server->shared));
			if (server->shared == NULL) {
				o.io->remove_iteration_hook(o.io, server->hook);
				protobuf_c_grpc2__server_release_auth(server);
				free(server->services);
				free(server);
				return NULL;
			}
			server->shared->refs = 1;	/* the server's */
			server->shared->io = o.io;
			server->shared->home = pthread_self();
		}
		server->io = o.io;
		if (o.io->ref != NULL)
			o.io->ref(o.io);	/* else ownership is transferred */
		server->sts = o.secure_transport;
		if (o.secure_transport != NULL && o.secure_transport->ref != NULL)
			o.secure_transport->ref(o.secure_transport);
	}
	if (o.auth != NULL) {
		server->auth = o.auth;
		if (o.auth->ref != NULL)
			o.auth->ref(o.auth);	/* else ownership is transferred */
	}
	return server;
}

int
protobuf_c_grpc2_server_listen(ProtobufCGrpc2Server *server, const char *host,
			       unsigned port)
{
	void **l;
	void *listener = NULL;

	if (server->io == NULL || server->shutting_down)
		return -1;
	l = realloc(server->listeners, (server->n_listeners + 1) * sizeof(*l));
	if (l == NULL)
		return -1;
	server->listeners = l;
	if (server->io->listen(server->io, host, port, server_accepted, server,
			       &listener) != 0)
		return -1;
	l[server->n_listeners++] = listener;
	return 0;
}

void
protobuf_c_grpc2__server_io_free(ProtobufCGrpc2Server *server)
{
	ConnPump *p;
	size_t i;

	if (server->shutting_down)
		return;
	server->shutting_down = 1;
	for (i = 0; i < server->n_listeners; i++)
		server->io->unlisten(server->io, server->listeners[i]);
	server->n_listeners = 0;
	for (p = server->conns; p != NULL; p = p->next)
		protobuf_c_grpc2__pump_close(p);
	server_maybe_finish(server);
}
