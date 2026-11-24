/* Private to the core library. */
#ifndef PROTOBUF_C_GRPC2_INTERNAL_H
#define PROTOBUF_C_GRPC2_INTERNAL_H

#include "protobuf-c-grpc2.h"
#include "protobuf-c-grpc2-builder.h"

/* ---- connection pump (pump.c): moves bytes between an I/O stream, an
 * optional TLS session and a ProtobufCGrpc2Conn. Used by servers and by
 * clients. ---- */

typedef struct ConnPump ConnPump;

struct ConnPump {
	ProtobufCGrpc2IoSystem *io;
	void *stream;
	ProtobufCGrpc2Conn *conn;
	ProtobufCGrpc2SecureTransport *tls;
	void *timer;
	ConnPump *next, **pprev;	/* for the owner's use */
	int closing;
	int alpn_checked;
	int established;		/* application bytes were received */
	/* The stream is closed: on_closing is called before the connection is
	 * freed (so the owner can stop using it), and on_closed after
	 * everything has been freed. */
	void (*on_closing)(ConnPump *pump, void *arg);
	void (*on_closed)(void *arg, int established);
	void *arg;
};

/* Takes over `stream`, `conn` and `tls` (which may be NULL) and starts
 * reading. On failure returns NULL, having closed `stream` but not freed
 * `conn` or `tls`. */
ConnPump *protobuf_c_grpc2__pump_new(
	ProtobufCGrpc2IoSystem *io, void *stream, ProtobufCGrpc2Conn *conn,
	ProtobufCGrpc2SecureTransport *tls,
	void (*on_closing)(ConnPump *, void *),
	void (*on_closed)(void *, int), void *arg);
void protobuf_c_grpc2__pump_flush(ConnPump *pump);
void protobuf_c_grpc2__pump_close(ConnPump *pump);

/* ---- servers ---- */

struct ProtobufCGrpc2Server {
	ProtobufCService **services;
	size_t n_services;

	/* Set up by protobuf_c_grpc2_server_from_builder(); NULL io means a
	 * plain registry for the sans-I/O API. */
	ProtobufCGrpc2IoSystem *io;
	ProtobufCGrpc2SecureTransportSystem *sts;
	ProtobufCGrpc2ConnOptions conn_options;
	ConnPump *conns;
	size_t n_conns;
	void **listeners;
	size_t n_listeners;
	void *hook;
	int shutting_down;
};

/* server.c */
void protobuf_c_grpc2__server_io_free(ProtobufCGrpc2Server *server);

/* ---- clients (client.c, used by protobuf-c-grpc2.c) ---- */

/* The connection to use for a new call, starting one if needed; NULL while
 * the client is backing off after a failed connection attempt. */
ProtobufCGrpc2Conn *protobuf_c_grpc2__client_acquire_conn(
	ProtobufCGrpc2Client *client);
const ProtobufCGrpc2ConnOptions *protobuf_c_grpc2__client_options(
	const ProtobufCGrpc2Client *client);

/* protobuf-c-grpc2.c: a client service handle bound to a client */
ProtobufCService *protobuf_c_grpc2__client_service_new(
	ProtobufCGrpc2Client *client, const ProtobufCServiceDescriptor *d,
	uint64_t timeout_ms);
void protobuf_c_grpc2__client_service_free(ProtobufCService *service);

#endif
