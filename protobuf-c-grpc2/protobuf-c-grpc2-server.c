/*
 * Server builder and the generic server connection pump: it moves bytes
 * between a pluggable ProtobufCGrpc2IoSystem, an optional pluggable
 * ProtobufCGrpc2SecureTransport and a ProtobufCGrpc2Conn.
 */
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2-internal.h"

#define IO_CHUNK 16384

/* ------------------------------------------------------------------ */
/* builder */

struct ProtobufCGrpc2ServerBuilder {
	ProtobufCService **services;
	size_t n_services;
	ProtobufCGrpc2ConnOptions options;
	ProtobufCGrpc2IoSystem *io;
	ProtobufCGrpc2SecureTransportSystem *sts;
};

ProtobufCGrpc2ServerBuilder *
protobuf_c_grpc2_server_builder_new(void)
{
	return calloc(1, sizeof(ProtobufCGrpc2ServerBuilder));
}

void
protobuf_c_grpc2_server_builder_free(ProtobufCGrpc2ServerBuilder *b)
{
	if (b == NULL)
		return;
	if (b->io != NULL && b->io->destroy != NULL)
		b->io->destroy(b->io);
	if (b->sts != NULL && b->sts->destroy != NULL)
		b->sts->destroy(b->sts);
	free(b->services);
	free(b);
}

int
protobuf_c_grpc2_server_builder_add_service(ProtobufCGrpc2ServerBuilder *b,
					    ProtobufCService *service)
{
	const char *name = service->descriptor->name;
	ProtobufCService **n;
	size_t i;

	if (name == NULL)
		return -1;
	for (i = 0; i < b->n_services; i++)
		if (strcmp(b->services[i]->descriptor->name, name) == 0)
			return -1;
	n = realloc(b->services, (b->n_services + 1) * sizeof(*n));
	if (n == NULL)
		return -1;
	b->services = n;
	n[b->n_services++] = service;
	return 0;
}

void
protobuf_c_grpc2_server_builder_set_options(ProtobufCGrpc2ServerBuilder *b,
					    const ProtobufCGrpc2ConnOptions *o)
{
	b->options = *o;
}

int
protobuf_c_grpc2_server_builder_add_io_system(ProtobufCGrpc2ServerBuilder *b,
					      ProtobufCGrpc2IoSystem *io)
{
	if (b->io != NULL)
		return -1;
	b->io = io;
	return 0;
}

int
protobuf_c_grpc2_server_builder_add_secure_transport_system(
	ProtobufCGrpc2ServerBuilder *b, ProtobufCGrpc2SecureTransportSystem *sts)
{
	if (b->sts != NULL)
		return -1;
	b->sts = sts;
	return 0;
}

/* ------------------------------------------------------------------ */
/* per-connection pump */

struct ServerConn {
	ProtobufCGrpc2Server *server;
	void *stream;
	ProtobufCGrpc2Conn *conn;
	ProtobufCGrpc2SecureTransport *tls;
	void *timer;
	ServerConn *next, **pprev;
	int closing;
	int alpn_checked;
};

static void sc_close(ServerConn *sc);

static void
sc_timer_cb(void *data)
{
	ServerConn *sc = data;
	if (!sc->closing && protobuf_c_grpc2_conn_handle_expiry(sc->conn) < 0)
		sc_close(sc);
}

/* 1 when application data may flow; closes the connection on failure. */
static int
sc_tls_ready(ServerConn *sc)
{
	int st = sc->tls->vtable->handshake_state(sc->tls);

	if (st < 0) {
		sc_close(sc);
		return 0;
	}
	if (st == 0)
		return 0;
	if (!sc->alpn_checked) {
		const char *alpn = sc->tls->vtable->alpn(sc->tls);
		if (alpn == NULL || strcmp(alpn, "h2") != 0) {
			sc_close(sc);	/* gRPC needs HTTP/2 */
			return 0;
		}
		sc->alpn_checked = 1;
	}
	return 1;
}

static void
sc_received(void *data, const uint8_t *bytes, size_t len)
{
	ServerConn *sc = data;
	uint8_t buf[IO_CHUNK];

	if (sc->closing)
		return;
	if (sc->tls == NULL) {
		if (protobuf_c_grpc2_conn_read(sc->conn, bytes, len) < 0)
			sc_close(sc);
		return;
	}
	if (sc->tls->vtable->feed_ciphertext(sc->tls, bytes, len) < 0) {
		sc_close(sc);
		return;
	}
	if (!sc_tls_ready(sc))
		return;
	for (;;) {
		ssize_t n = sc->tls->vtable->read_plaintext(sc->tls, buf, sizeof buf);
		if (n < 0) {
			sc_close(sc);
			return;
		}
		if (n == 0)
			return;
		if (protobuf_c_grpc2_conn_read(sc->conn, buf, (size_t) n) < 0 ||
		    sc->closing) {
			sc_close(sc);
			return;
		}
	}
}

static void
sc_eof(void *data, int error)
{
	(void) error;
	sc_close(data);
}

/* Send everything the connection (and TLS session) has queued. */
static void
sc_flush(ServerConn *sc)
{
	ProtobufCGrpc2IoSystem *io = sc->server->io;
	uint8_t buf[IO_CHUNK];
	ssize_t n;
	int64_t t;

	if (sc->closing)
		return;
	if (sc->tls == NULL) {
		while ((n = protobuf_c_grpc2_conn_write(sc->conn, buf, sizeof buf)) > 0)
			if (io->stream_write(io, sc->stream, buf, (size_t) n) != 0) {
				sc_close(sc);
				return;
			}
		if (n < 0) {
			sc_close(sc);
			return;
		}
	} else {
		if (sc_tls_ready(sc)) {
			while ((n = protobuf_c_grpc2_conn_write(sc->conn, buf, sizeof buf)) > 0)
				if (sc->tls->vtable->write_plaintext(sc->tls, buf,
								     (size_t) n) < 0) {
					sc_close(sc);
					return;
				}
			if (n < 0) {
				sc_close(sc);
				return;
			}
		}
		if (sc->closing)
			return;
		while ((n = sc->tls->vtable->read_ciphertext(sc->tls, buf,
							     sizeof buf)) > 0)
			if (io->stream_write(io, sc->stream, buf, (size_t) n) != 0) {
				sc_close(sc);
				return;
			}
	}
	t = protobuf_c_grpc2_conn_get_timeout_ns(sc->conn);
	if (t < 0)
		io->timer_stop(io, sc->timer);
	else
		io->timer_start(io, sc->timer, (uint64_t) t / 1000000u + 1);
}

static void
server_maybe_finish(ProtobufCGrpc2Server *server)
{
	if (!server->shutting_down || server->n_conns > 0)
		return;
	if (server->hook != NULL)
		server->io->remove_iteration_hook(server->io, server->hook);
	if (server->io->destroy != NULL)
		server->io->destroy(server->io);
	if (server->sts != NULL && server->sts->destroy != NULL)
		server->sts->destroy(server->sts);
	free(server->listeners);
	free(server->services);
	free(server);
}

static void
sc_closed(void *data)
{
	ServerConn *sc = data;
	ProtobufCGrpc2Server *server = sc->server;

	server->io->timer_free(server->io, sc->timer);
	if (sc->tls != NULL)
		sc->tls->vtable->destroy(sc->tls);
	protobuf_c_grpc2_conn_free(sc->conn);
	*sc->pprev = sc->next;
	if (sc->next != NULL)
		sc->next->pprev = sc->pprev;
	free(sc);
	server->n_conns--;
	server_maybe_finish(server);
}

static void
sc_close(ServerConn *sc)
{
	if (sc->closing)
		return;
	sc->closing = 1;
	sc->server->io->stream_close(sc->server->io, sc->stream);
}

static void
server_iteration_hook(void *data)
{
	ProtobufCGrpc2Server *server = data;
	ServerConn *sc;

	for (sc = server->conns; sc != NULL; sc = sc->next)
		sc_flush(sc);
}

static void
server_accepted(void *data, void *stream)
{
	ProtobufCGrpc2Server *server = data;
	ProtobufCGrpc2IoSystem *io = server->io;
	ProtobufCGrpc2IoHandler h;
	ServerConn *sc;

	sc = calloc(1, sizeof(*sc));
	if (sc == NULL || server->shutting_down) {
		free(sc);
		io->stream_close(io, stream);
		return;
	}
	sc->server = server;
	sc->stream = stream;
	sc->conn = protobuf_c_grpc2_conn_server_new(server, &server->conn_options);
	if (server->sts != NULL)
		sc->tls = server->sts->new_server(server->sts);
	sc->timer = io->timer_new(io, sc_timer_cb, sc);
	if (sc->conn == NULL || sc->timer == NULL ||
	    (server->sts != NULL && sc->tls == NULL)) {
		if (sc->timer != NULL)
			io->timer_free(io, sc->timer);
		if (sc->tls != NULL)
			sc->tls->vtable->destroy(sc->tls);
		if (sc->conn != NULL)
			protobuf_c_grpc2_conn_free(sc->conn);
		free(sc);
		io->stream_close(io, stream);
		return;
	}
	sc->next = server->conns;
	sc->pprev = &server->conns;
	if (server->conns != NULL)
		server->conns->pprev = &sc->next;
	server->conns = sc;
	server->n_conns++;

	h.received = sc_received;
	h.eof = sc_eof;
	h.closed = sc_closed;
	h.data = sc;
	if (io->stream_start(io, stream, &h) != 0)
		sc_close(sc);
}

/* ------------------------------------------------------------------ */
/* server */

ProtobufCGrpc2Server *
protobuf_c_grpc2_server_from_builder(ProtobufCGrpc2ServerBuilder *b)
{
	ProtobufCGrpc2Server *server;

	if (b->io == NULL)
		return NULL;
	server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;
	if (b->n_services > 0) {
		server->services = malloc(b->n_services * sizeof(*server->services));
		if (server->services == NULL) {
			free(server);
			return NULL;
		}
		memcpy(server->services, b->services,
		       b->n_services * sizeof(*server->services));
		server->n_services = b->n_services;
	}
	server->conn_options = b->options;
	server->io = b->io;
	server->sts = b->sts;
	b->io = NULL;			/* ownership moves to the server */
	b->sts = NULL;
	server->hook = server->io->add_iteration_hook(
		server->io, server_iteration_hook, server);
	if (server->hook == NULL) {
		server->shutting_down = 1;
		server_maybe_finish(server);
		return NULL;
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
	ServerConn *sc;
	size_t i;

	if (server->shutting_down)
		return;
	server->shutting_down = 1;
	for (i = 0; i < server->n_listeners; i++)
		server->io->unlisten(server->io, server->listeners[i]);
	server->n_listeners = 0;
	for (sc = server->conns; sc != NULL; sc = sc->next)
		sc_close(sc);
	server_maybe_finish(server);
}
