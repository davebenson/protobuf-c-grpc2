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
