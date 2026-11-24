/* Private to the core library. */
#ifndef PROTOBUF_C_GRPC2_INTERNAL_H
#define PROTOBUF_C_GRPC2_INTERNAL_H

#include "protobuf-c-grpc2.h"
#include "protobuf-c-grpc2-builder.h"

typedef struct ServerConn ServerConn;

struct ProtobufCGrpc2Server {
	ProtobufCService **services;
	size_t n_services;

	/* Set up by protobuf_c_grpc2_server_from_builder(); NULL io means a
	 * plain registry for the sans-I/O API. */
	ProtobufCGrpc2IoSystem *io;
	ProtobufCGrpc2SecureTransportSystem *sts;
	ProtobufCGrpc2ConnOptions conn_options;
	ServerConn *conns;
	size_t n_conns;
	void **listeners;
	size_t n_listeners;
	void *hook;
	int shutting_down;
};

/* server.c */
void protobuf_c_grpc2__server_io_free(ProtobufCGrpc2Server *server);

#endif
