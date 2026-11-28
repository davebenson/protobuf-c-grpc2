/*
 * background-rpc: run gRPC in a thread of its own that the rest of the program
 * can ignore.
 *
 *   usage: background-rpc [port [cert.pem key.pem]]
 *
 * main() starts the RPC thread and waits until it says it is ready. The RPC
 * thread creates the event loop, the I/O system, the (optional) TLS system, a
 * server and a client, and then runs the loop for as long as the program
 * lives. Everything else is ordinary threaded code: any number of threads call
 * the client's service handle, and the server could answer from a thread pool
 * (both are created with `thread_safe`).
 *
 * The rules this relies on:
 *   - the thread that creates the client and the server must be the one that
 *     runs the loop (here: the RPC thread does both);
 *   - closures and on_call_done run on the RPC thread, so a caller that
 *     wants to wait for a reply hands over the result from its closure (here
 *     with a little Waiter), and does not do anything slow in there;
 *   - to stop, ask the RPC thread to do it, with the I/O system's `post`.
 *
 * With a certificate and key the server uses TLS, and the client verifies it
 * (the certificate must be valid for "localhost"). Needs OpenSSL.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <protobuf-c-grpc2/protobuf-c-grpc2.h>
#include <protobuf-c-grpc2/protobuf-c-grpc2-poll.h>
#ifdef HAVE_OPENSSL
#include <protobuf-c-grpc2/protobuf-c-grpc2-openssl.h>
#endif
#include "greeter.pb-c.h"

/* ---- a latch: main() waits for the RPC thread to be ready ---- */
/* (A POSIX semaphore would do; unnamed ones are missing on macOS.) */

typedef struct {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int signalled;
} Latch;

static void
latch_init(Latch *l)
{
	pthread_mutex_init(&l->mutex, NULL);
	pthread_cond_init(&l->cond, NULL);
	l->signalled = 0;
}

static void
latch_signal(Latch *l)
{
	pthread_mutex_lock(&l->mutex);
	l->signalled = 1;
	pthread_cond_broadcast(&l->cond);
	pthread_mutex_unlock(&l->mutex);
}

static void
latch_wait(Latch *l)
{
	pthread_mutex_lock(&l->mutex);
	while (!l->signalled)
		pthread_cond_wait(&l->cond, &l->mutex);
	pthread_mutex_unlock(&l->mutex);
}

/* ---- the service the server offers (runs on the RPC thread) ---- */

static void
greeter_say_hello(Example__Greeter_Service *s, const Example__HelloRequest *req,
		  Example__HelloReply_Closure closure, void *closure_data)
{
	Example__HelloReply reply = EXAMPLE__HELLO_REPLY__INIT;
	char text[256];
	(void) s;

	/* Cheap enough to answer here. For slow work, hand `closure` and a
	 * copy of the request to a worker thread and let it call
	 * closure(&reply, closure_data) from there: the server was created
	 * with thread_safe, so that is allowed. */
	snprintf(text, sizeof text, "Hello, %s!", req->name);
	reply.message = text;
	closure(&reply, closure_data);
}

static void
greeter_say_hello_many(Example__Greeter_Service *s,
		       const Example__HelloRequest *req,
		       Example__HelloReply_Closure closure, void *closure_data)
{
	(void) s; (void) req;
	closure(NULL, closure_data);		/* not implemented here */
}

static ProtobufCInputStream *
greeter_collect(Example__Greeter_Service *s, const Example__HelloRequest *first,
		Example__HelloReply_Closure closure, void *closure_data)
{
	(void) s; (void) first;
	closure(NULL, closure_data);
	return NULL;
}

/* ---- the RPC thread ---- */

typedef struct {
	/* in */
	unsigned port;
	const char *cert_file, *key_file;	/* NULL: no TLS */
	/* out, once `ready` is signalled */
	Latch ready;
	int failed;				/* it could not start */
	ProtobufCService *greeter;		/* client handle: any thread */
	ProtobufCGrpc2IoSystem *io;		/* for post() */
	/* the RPC thread's own */
	pthread_t thread;
	ProtobufCGrpc2PollLoop *loop;
	ProtobufCGrpc2Server *server;
	ProtobufCGrpc2Client *client;
} RpcThread;

#ifdef HAVE_OPENSSL
static ProtobufCGrpc2SecureTransportSystem *
make_server_tls(const RpcThread *r)
{
	ProtobufCGrpc2SecureTransportSystem *sts;
	ProtobufCGrpc2OpensslOptions oo = PROTOBUF_C_GRPC2_OPENSSL_OPTIONS_INIT;
	SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());

	if (SSL_CTX_use_certificate_chain_file(ctx, r->cert_file) != 1 ||
	    SSL_CTX_use_PrivateKey_file(ctx, r->key_file, SSL_FILETYPE_PEM) != 1) {
		SSL_CTX_free(ctx);
		return NULL;
	}
	oo.ctx = ctx;
	sts = protobuf_c_grpc2_secure_transport_system_new_openssl(&oo);
	SSL_CTX_free(ctx);	/* the transport holds its own reference */
	return sts;
}

static ProtobufCGrpc2SecureTransportSystem *
make_client_tls(const RpcThread *r)
{
	ProtobufCGrpc2SecureTransportSystem *sts;
	ProtobufCGrpc2OpensslOptions oo = PROTOBUF_C_GRPC2_OPENSSL_OPTIONS_INIT;
	SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());

	/* the server's own certificate stands in for a CA */
	if (SSL_CTX_load_verify_locations(ctx, r->cert_file, NULL) != 1) {
		SSL_CTX_free(ctx);
		return NULL;
	}
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
	oo.ctx = ctx;
	sts = protobuf_c_grpc2_secure_transport_system_new_openssl(&oo);
	SSL_CTX_free(ctx);
	return sts;
}
#endif

static void
rpc_thread_start_failed(RpcThread *r)
{
	r->failed = 1;
	latch_signal(&r->ready);
}

static void *
rpc_thread_main(void *arg)
{
	RpcThread *r = arg;
	Example__Greeter_Service greeter = EXAMPLE__GREETER__INIT(greeter_);
	ProtobufCService *services[] = { &greeter.base };
	ProtobufCGrpc2ServerOptions so = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
	ProtobufCGrpc2ClientOptions co = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
	ProtobufCGrpc2PollOptions po = PROTOBUF_C_GRPC2_POLL_OPTIONS_INIT;

	/* 1. the event loop and the I/O system that runs on it. One system
	 *    serves the server and the client: it is shared, and freed when
	 *    both are gone. */
	r->loop = protobuf_c_grpc2_poll_loop_new();
	if (r->loop == NULL) {
		rpc_thread_start_failed(r);
		return NULL;
	}
	po.loop = r->loop;
	r->io = protobuf_c_grpc2_io_system_new_poll(&po);
	if (r->io == NULL) {
		rpc_thread_start_failed(r);
		return NULL;
	}

	/* 2. the TLS systems, if wanted */
	so.secure_transport = NULL;
	co.secure_transport = NULL;
#ifdef HAVE_OPENSSL
	if (r->cert_file != NULL) {
		so.secure_transport = make_server_tls(r);
		co.secure_transport = make_client_tls(r);
		if (so.secure_transport == NULL || co.secure_transport == NULL) {
			fprintf(stderr, "cannot set up TLS\n");
			rpc_thread_start_failed(r);
			return NULL;
		}
	}
#else
	if (r->cert_file != NULL) {
		fprintf(stderr, "built without OpenSSL: no TLS\n");
		rpc_thread_start_failed(r);
		return NULL;
	}
#endif

	/* 3. the server, listening, and the client of the same service.
	 *    thread_safe: the client may be called, and the server's closures
	 *    answered, from any thread. */
	so.services = services;
	so.n_services = 1;
	so.io = r->io;
	so.thread_safe = 1;
	r->server = protobuf_c_grpc2_server_new(&so);
	if (r->server == NULL ||
	    protobuf_c_grpc2_server_listen(r->server, "127.0.0.1", r->port) != 0) {
		fprintf(stderr, "cannot listen on port %u\n", r->port);
		rpc_thread_start_failed(r);
		return NULL;
	}
	co.host = "127.0.0.1";
	co.server_name = "localhost";		/* what the certificate says */
	co.port = r->port;
	co.io = r->io;
	co.thread_safe = 1;
	r->client = protobuf_c_grpc2_client_new(&co);
	if (r->client == NULL) {
		rpc_thread_start_failed(r);
		return NULL;
	}
	r->greeter = protobuf_c_grpc2_client_get_service(
		r->client, &example__greeter__descriptor);

	/* 4. tell main() it can go on, then serve until asked to stop */
	latch_signal(&r->ready);
	protobuf_c_grpc2_poll_loop_run(r->loop);
	protobuf_c_grpc2_poll_loop_close(r->loop);
	return NULL;
}

static int
rpc_thread_start(RpcThread *r, unsigned port, const char *cert, const char *key)
{
	memset(r, 0, sizeof(*r));
	r->port = port;
	r->cert_file = cert;
	r->key_file = key;
	latch_init(&r->ready);
	if (pthread_create(&r->thread, NULL, rpc_thread_main, r) != 0)
		return -1;
	latch_wait(&r->ready);
	if (r->failed) {
		pthread_join(r->thread, NULL);
		return -1;
	}
	return 0;
}

/* runs on the RPC thread: free the client and the server, and the loop, which
 * has nothing left to wait for, returns */
static void
rpc_thread_shutdown(void *arg)
{
	RpcThread *r = arg;

	protobuf_c_grpc2_client_free(r->client);
	protobuf_c_grpc2_server_free(r->server);
}

static void
rpc_thread_stop(RpcThread *r)
{
	r->io->post(r->io, rpc_thread_shutdown, r);
	pthread_join(r->thread, NULL);
}

/* ---- using it from ordinary threads ---- */

/* Waits for one reply: the closure runs on the RPC thread, the caller
 * sleeps. */
typedef struct {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int done;
	int got_reply;
	char text[256];
} Waiter;

static void
on_reply(const Example__HelloReply *reply, void *data)
{
	Waiter *w = data;

	pthread_mutex_lock(&w->mutex);
	if (reply != NULL) {
		snprintf(w->text, sizeof w->text, "%s", reply->message);
		w->got_reply = 1;
	}
	w->done = 1;		/* a unary call's closure is called once */
	pthread_cond_broadcast(&w->cond);
	pthread_mutex_unlock(&w->mutex);
}

typedef struct {
	RpcThread *rpc;
	int id;
	int failures;
} User;

static void *
user_thread(void *arg)
{
	User *u = arg;
	int i;

	for (i = 0; i < 3; i++) {
		Example__HelloRequest req = EXAMPLE__HELLO_REQUEST__INIT;
		Waiter w;
		char name[64], want[96];

		snprintf(name, sizeof name, "user-%d-%d", u->id, i);
		req.name = name;
		memset(&w, 0, sizeof w);
		pthread_mutex_init(&w.mutex, NULL);
		pthread_cond_init(&w.cond, NULL);

		/* from this thread, as from any: the request is copied and
		 * handed to the RPC thread */
		example__greeter__say_hello(u->rpc->greeter, &req, on_reply, &w);

		pthread_mutex_lock(&w.mutex);
		while (!w.done)
			pthread_cond_wait(&w.cond, &w.mutex);
		pthread_mutex_unlock(&w.mutex);

		snprintf(want, sizeof want, "Hello, %s!", name);
		if (!w.got_reply || strcmp(w.text, want) != 0) {
			u->failures++;
			printf("thread %d: FAILED %s\n", u->id, name);
		} else {
			printf("thread %d: %s\n", u->id, w.text);
		}
		pthread_mutex_destroy(&w.mutex);
		pthread_cond_destroy(&w.cond);
	}
	return NULL;
}

int
main(int argc, char **argv)
{
	enum { N_USERS = 4 };
	unsigned port = argc > 1 ? (unsigned) atoi(argv[1])
				 : 20000u + (unsigned) getpid() % 20000u;
	const char *cert = argc > 3 ? argv[2] : NULL;
	const char *key = argc > 3 ? argv[3] : NULL;
	RpcThread rpc;
	pthread_t th[N_USERS];
	User users[N_USERS];
	int i, failures = 0;

	if (rpc_thread_start(&rpc, port, cert, key) != 0)
		return 2;
	printf("main: RPC thread is up on port %u%s\n", port,
	       cert ? " (TLS)" : "");

	/* from here on RPC is just a handle that any thread can call */
	for (i = 0; i < N_USERS; i++) {
		users[i].rpc = &rpc;
		users[i].id = i;
		users[i].failures = 0;
		pthread_create(&th[i], NULL, user_thread, &users[i]);
	}
	for (i = 0; i < N_USERS; i++) {
		pthread_join(th[i], NULL);
		failures += users[i].failures;
	}

	rpc_thread_stop(&rpc);
	printf("main: %s\n", failures ? "FAILED" : "ok");
	return failures != 0;
}
