/*
 * Servers that run their own connections: creation, the listeners and the
 * list of connections. The per-connection work (moving bytes between the I/O
 * stream, the optional TLS session and the HTTP/2 connection) is in pump.c.
 */
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2-internal.h"

#define IO_CHUNK 16384

/* ------------------------------------------------------------------ */
/* connections (the per-connection work is in pump.c) */

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
			free(server->services);
			free(server);
			return NULL;
		}
		server->io = o.io;
		if (o.io->ref != NULL)
			o.io->ref(o.io);	/* else ownership is transferred */
		server->sts = o.secure_transport;
		if (o.secure_transport != NULL && o.secure_transport->ref != NULL)
			o.secure_transport->ref(o.secure_transport);
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
