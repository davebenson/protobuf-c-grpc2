/*
 * libuv-client: a trivial gRPC client using libuv and protobuf-c-grpc2.
 *
 *   usage: libuv-client [host-ip [port [name]]]
 *          (defaults: 127.0.0.1 50051 world)
 *
 * Connects to libuv-server and makes three calls at the same time over the
 * one HTTP/2 connection: SayHello (unary), SayHelloMany (server streaming)
 * and Collect (client streaming). Exits when all three have finished; the
 * exit status is nonzero if any call failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv-grpc2.h"
#include "greeter.pb-c.h"

static UvConn *the_conn;
static int calls_pending;
static int calls_failed;
static ProtobufCService *greeter;
static const char *name = "world";

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
	/* Closing from inside a grpc2 callback is safe: uvconn_close() only
	 * starts closing the libuv handles, and the ProtobufCGrpc2Conn is freed from
	 * their close callbacks, after we have returned. When the connection
	 * is gone nothing keeps the loop alive and uv_run() returns. */
	if (--calls_pending == 0)
		uvconn_close(the_conn);
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
start_calls(UvConn *c)
{
	Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
	ProtobufCInputStream *in;
	static const char *const friends[] = { "alice", "bob", "carol" };
	size_t i;

	greeter = protobuf_c_grpc2_conn_client_service(c->conn,
					    &example__greeter__descriptor);
	calls_pending = 3;	/* set up front: calls may finish synchronously */

	/* unary */
	req.name = (char *) name;
	example__greeter__say_hello(greeter, &req, on_reply, "SayHello");

	/* server streaming */
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

static void
on_connect(uv_connect_t *req, int status)
{
	UvConn *c = req->data;

	free(req);
	if (status < 0) {
		fprintf(stderr, "connect: %s\n", uv_strerror(status));
		calls_failed++;
		uvconn_close(c);
		return;
	}
	uvconn_start(c);
	start_calls(c);
}

int
main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	int port = argc > 2 ? atoi(argv[2]) : 50051;
	uv_loop_t *loop = uv_default_loop();
	struct sockaddr_in addr;
	ProtobufCGrpc2ConnOptions opts;
	uv_connect_t *creq;
	UvConn *c;

	if (argc > 3)
		name = argv[3];
	if (uv_ip4_addr(host, port, &addr) != 0) {
		fprintf(stderr, "bad address %s:%d\n", host, port);
		return 2;
	}
	uvconn_loop_init(loop);

	memset(&opts, 0, sizeof opts);
	opts.on_call_done = on_call_done;
	opts.authority = host;
	c = uvconn_new(loop, protobuf_c_grpc2_conn_client_new(&opts));
	if (c == NULL)
		return 1;
	the_conn = c;

	creq = malloc(sizeof(*creq));
	creq->data = c;
	uv_tcp_connect(creq, &c->tcp, (const struct sockaddr *) &addr, on_connect);
	uv_run(loop, UV_RUN_DEFAULT);
	return calls_failed != 0;
}
