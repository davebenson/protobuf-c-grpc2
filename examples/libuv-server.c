/*
 * libuv-server: a trivial gRPC server using libuv and protobuf-c-grpc2.
 *
 *   usage: libuv-server [port [cert.pem key.pem]]   (default port 50051)
 *
 * Implements the Greeter service from greeter.proto:
 *   SayHello      unary
 *   SayHelloMany  server streaming; replies come from a libuv timer, which
 *                 shows a handler answering asynchronously
 *   Collect       client streaming; greets everybody in the request stream
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include <protobuf-c-grpc2/protobuf-c-grpc2-builder.h>
#include <protobuf-c-grpc2/protobuf-c-grpc2-uv.h>
#ifdef HAVE_OPENSSL
#include <protobuf-c-grpc2/protobuf-c-grpc2-openssl.h>
#endif
#include "greeter.pb-c.h"

/* ---- SayHello: unary ---- */

static void
greeter_say_hello(Example__Greeter_Service *service, const Example__HelloRequest *req,
	  Example__HelloReply_Closure closure, void *closure_data)
{
	Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
	char text[256];
	(void) service;

	snprintf(text, sizeof text, "Hello, %s!", req->name);
	reply.message = text;
	closure(&reply, closure_data);	/* a unary reply ends the call */
}

/* ---- SayHelloMany: server streaming, driven by a timer ---- */

typedef struct {
	uv_timer_t timer;
	char *name;
	int sent, total;
	Example__HelloReply_Closure closure;
	void *closure_data;
} ManyState;

static void
many_closed(uv_handle_t *h)
{
	ManyState *st = (ManyState *) h;
	free(st->name);
	free(st);
}

static void
many_tick(uv_timer_t *t)
{
	ManyState *st = (ManyState *) t;

	if (st->sent < st->total) {
		Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
		char text[256];
		snprintf(text, sizeof text, "Hello #%d, %s!", ++st->sent, st->name);
		reply.message = text;
		st->closure(&reply, st->closure_data);
		return;
	}
	/* NULL ends the stream. If the client went away meanwhile, grpc2
	 * ignores everything we send but still needs this final call. */
	st->closure(NULL, st->closure_data);
	uv_close((uv_handle_t *) &st->timer, many_closed);
}

static void
greeter_say_hello_many(Example__Greeter_Service *service,
	       const Example__HelloRequest *req,
	       Example__HelloReply_Closure closure, void *closure_data)
{
	ManyState *st = calloc(1, sizeof(*st));
	(void) service;

	st->name = strdup(req->name);
	st->total = req->count;
	st->closure = closure;
	st->closure_data = closure_data;
	uv_timer_init(uv_default_loop(), &st->timer);
	uv_timer_start(&st->timer, many_tick, 200, 200);
}

/* ---- Collect: client streaming ---- */

typedef struct {
	ProtobufCInputStream base;	/* must be first */
	char *names;			/* "a, b, c" */
	Example__HelloReply_Closure closure;
	void *closure_data;
} CollectStream;

static protobuf_c_boolean
collect_send_input(ProtobufCInputStream *stream, const ProtobufCMessage *msg)
{
	CollectStream *cs = (CollectStream *) stream;

	if (msg != NULL) {
		const Example__HelloRequest *req = (const Example__HelloRequest *) msg;
		size_t old = cs->names ? strlen(cs->names) : 0;
		size_t len = old + strlen(req->name) + 3;
		cs->names = realloc(cs->names, len);
		snprintf(cs->names + old, len - old, "%s%s", old ? ", " : "",
			 req->name);
	} else {
		/* end of the request stream: send the one reply */
		Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
		char text[1024];
		snprintf(text, sizeof text, "Hello, %s!",
			 cs->names ? cs->names : "nobody");
		reply.message = text;
		cs->closure(&reply, cs->closure_data);
	}
	return 1;
}

static void
collect_destroy(ProtobufCInputStream *stream)
{
	CollectStream *cs = (CollectStream *) stream;
	free(cs->names);
	free(cs);
}

static const ProtobufCInputStreamVTable collect_vtable = {
	collect_send_input,
	collect_destroy
};

static ProtobufCInputStream *
greeter_collect(Example__Greeter_Service *service, const Example__HelloRequest *first,
	Example__HelloReply_Closure closure, void *closure_data)
{
	CollectStream *cs = calloc(1, sizeof(*cs));
	(void) service;

	cs->base.vtable = &collect_vtable;
	cs->closure = closure;
	cs->closure_data = closure_data;
	if (first != NULL)			/* the optional first message */
		collect_send_input(&cs->base, &first->base);
	return &cs->base;
}

/* ---- assembling the server ---- */

static void
on_call_done(const ProtobufCMethodDescriptor *method, int status,
	     const char *message, void *user_data)
{
	(void) user_data;
	fprintf(stderr, "server: %s finished: %s%s%s\n",
		method ? method->name : "(unknown method)",
		protobuf_c_grpc2_status_name(status), message ? " - " : "",
		message ? message : "");
}

int
main(int argc, char **argv)
{
	/* The INIT macro takes a prefix and appends the lower-cased method
	 * names: greeter_say_hello, greeter_say_hello_many, greeter_collect. */
	Example__Greeter_Service greeter = EXAMPLE__GREETER__INIT(greeter_);
	uv_loop_t *loop = uv_default_loop();
	int port = argc > 1 ? atoi(argv[1]) : 50051;
	ProtobufCGrpc2ConnOptions opts;
	ProtobufCGrpc2ServerBuilder *builder;
	ProtobufCGrpc2Server *server;

	builder = protobuf_c_grpc2_server_builder_new();
	protobuf_c_grpc2_server_builder_add_service(builder, &greeter.base);
	protobuf_c_grpc2_server_builder_use_uv(builder, loop);

	memset(&opts, 0, sizeof opts);
	opts.on_call_done = on_call_done;
	protobuf_c_grpc2_server_builder_set_options(builder, &opts);

	if (argc > 3) {
#ifdef HAVE_OPENSSL
		/* Optional TLS: libuv-server PORT CERT.pem KEY.pem */
		SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
		if (SSL_CTX_use_certificate_chain_file(ctx, argv[2]) != 1 ||
		    SSL_CTX_use_PrivateKey_file(ctx, argv[3], SSL_FILETYPE_PEM) != 1) {
			fprintf(stderr, "cannot load certificate/key\n");
			return 1;
		}
		protobuf_c_grpc2_server_builder_use_openssl(builder, ctx);
		SSL_CTX_free(ctx);	/* the builder holds its own reference */
#else
		fprintf(stderr, "built without OpenSSL: no TLS\n");
		return 1;
#endif
	}

	server = protobuf_c_grpc2_server_from_builder(builder);
	protobuf_c_grpc2_server_builder_free(builder);
	if (server == NULL ||
	    protobuf_c_grpc2_server_listen(server, "127.0.0.1", (unsigned) port) != 0) {
		fprintf(stderr, "cannot listen on port %d\n", port);
		return 1;
	}
	fprintf(stderr, "server: listening on 127.0.0.1:%d%s\n", port,
		argc > 3 ? " (TLS)" : "");
	uv_run(loop, UV_RUN_DEFAULT);
	protobuf_c_grpc2_server_free(server);
	uv_run(loop, UV_RUN_DEFAULT);	/* let the shutdown complete */
	return 0;
}
