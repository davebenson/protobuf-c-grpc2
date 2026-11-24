/*
 * libuv-client: a trivial gRPC client using libuv and protobuf-c-grpc2.
 *
 *   usage: libuv-client [host [port [name [ca.pem]]]]
 *          (defaults: 127.0.0.1 50051 world, and no TLS)
 *
 * Connects to libuv-server and makes three calls at the same time over the
 * one HTTP/2 connection: SayHello (unary), SayHelloMany (server streaming)
 * and Collect (client streaming). Exits when all three have finished; the
 * exit status is nonzero if any call failed.
 *
 * With a fourth argument the connection uses TLS, verifying the server's
 * certificate against that CA file and the name given as `host`.
 */
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

static ProtobufCGrpc2Client *client;
static uv_timer_t finish_timer;
static int calls_pending;
static int calls_failed;
static const char *name = "world";

static void
finish_cb(uv_timer_t *t)
{
	/* Freeing the client from inside one of its own callbacks would be
	 * unwise, so this runs from a timer on the next loop turn. */
	(void) t;
	protobuf_c_grpc2_client_free(client);
	uv_close((uv_handle_t *) &finish_timer, NULL);
}

/* Called once for every finished call, whatever its type. This is also
 * where errors show up: closures only see a NULL message. */
static void
on_call_done(const ProtobufCMethodDescriptor *method, int status,
	     const char *message, void *user_data)
{
	(void) user_data;

	printf("client: %s finished: %s%s%s\n", method->name,
	       protobuf_c_grpc2_status_name(status), message ? " - " : "",
	       message ? message : "");
	if (status != PROTOBUF_C_GRPC2_OK)
		calls_failed++;
	if (--calls_pending == 0)
		uv_timer_start(&finish_timer, finish_cb, 0, 0);
}

static void
on_reply(const Example__HelloReply *reply, void *closure_data)
{
	const char *what = closure_data;

	if (reply == NULL)		/* end of stream (or failure) */
		return;
	printf("client: %s reply: %s\n", what, reply->message);
}

static void
start_calls(void)
{
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
	ProtobufCService *greeter = protobuf_c_grpc2_client_get_service(
		client, &example__greeter__descriptor);
	ProtobufCInputStream *in;
	static const char *const friends[] = { "alice", "bob", "carol" };
	size_t i;

	calls_pending = 3;	/* set up front: calls may finish synchronously */

	/* The first call makes the client connect. Calls made before the
	 * connection is up are queued. */
	req.name = (char *) name;
	example__greeter__say_hello(greeter, &req, on_reply, "SayHello");

	req.count = 3;
	example__greeter__say_hello_many(greeter, &req, on_reply, "SayHelloMany");

	/* client streaming: returns the stream to send requests on */
	in = example__greeter__collect(greeter, NULL, on_reply, "Collect");
	for (i = 0; in != NULL && i < sizeof friends / sizeof friends[0]; i++) {
		req.name = (char *) friends[i];
		in->vtable->send_input(in, &req.base);
	}
	if (in != NULL) {
		in->vtable->send_input(in, NULL);	/* end of requests */
		in->vtable->destroy(in);
	}
}

int
main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	unsigned port = argc > 2 ? (unsigned) atoi(argv[2]) : 50051;
	uv_loop_t *loop = uv_default_loop();
	ProtobufCGrpc2ConnOptions opts;
	ProtobufCGrpc2ClientBuilder *builder;

	if (argc > 3)
		name = argv[3];

	builder = protobuf_c_grpc2_client_builder_new();
	protobuf_c_grpc2_client_builder_set_target(builder, host, port);
	protobuf_c_grpc2_client_builder_use_uv(builder, loop);
	memset(&opts, 0, sizeof opts);
	opts.on_call_done = on_call_done;
	protobuf_c_grpc2_client_builder_set_options(builder, &opts);

	if (argc > 4) {
#ifdef HAVE_OPENSSL
		SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
		if (SSL_CTX_load_verify_locations(ctx, argv[4], NULL) != 1) {
			fprintf(stderr, "cannot load CA file %s\n", argv[4]);
			return 2;
		}
		SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
		protobuf_c_grpc2_client_builder_use_openssl(builder, ctx);
		SSL_CTX_free(ctx);	/* the builder holds its own reference */
#else
		fprintf(stderr, "built without OpenSSL: no TLS\n");
		return 2;
#endif
	}

	client = protobuf_c_grpc2_client_from_builder(builder);
	protobuf_c_grpc2_client_builder_free(builder);
	if (client == NULL) {
		fprintf(stderr, "cannot create the client\n");
		return 2;
	}
	uv_timer_init(loop, &finish_timer);
	start_calls();
	uv_run(loop, UV_RUN_DEFAULT);
	return calls_failed != 0;
}
