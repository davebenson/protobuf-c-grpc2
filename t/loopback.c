/*
 * Loopback test: a grpc2 client connection and a grpc2 server connection
 * are connected in memory and driven by a simple pump.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
#include "t/loopback.pb-c.h"

/* ---- server implementation ---- */

typedef struct {
	ProtobufCInputStream base;
	int sum;
	int echo;
	int destroyed;
	ProtobufCClosure closure;
	void *closure_data;
} ServerStream;

static int server_streams_destroyed;

static void
impl_unary(Loopback__Counter_Service *s, const Loopback__Req *in,
	   Loopback__Rsp_Closure cl, void *d)
{
	Loopback__Rsp rsp = LOOPBACK__RSP__INIT;
	(void) s;
	rsp.n = in->n + 1;
	rsp.text = in->text;
	cl(&rsp, d);
}

static void
impl_down(Loopback__Counter_Service *s, const Loopback__Req *in,
	  Loopback__Rsp_Closure cl, void *d)
{
	int i;
	(void) s;
	for (i = in->n; i > 0; i--) {
		Loopback__Rsp rsp = LOOPBACK__RSP__INIT;
		rsp.n = i;
		cl(&rsp, d);
	}
	cl(NULL, d);
}

static protobuf_c_boolean
ss_send_input(ProtobufCInputStream *st, const ProtobufCMessage *m)
{
	ServerStream *ss = (ServerStream *) st;
	if (m != NULL) {
		const Loopback__Req *r = (const Loopback__Req *) m;
		ss->sum += r->n;
		if (ss->echo) {
			Loopback__Rsp rsp = LOOPBACK__RSP__INIT;
			rsp.n = r->n * 10;
			ss->closure(&rsp.base, ss->closure_data);
		}
		return 1;
	}
	if (ss->echo) {
		ss->closure(NULL, ss->closure_data);
	} else {
		Loopback__Rsp rsp = LOOPBACK__RSP__INIT;
		rsp.n = ss->sum;
		ss->closure(&rsp.base, ss->closure_data);
	}
	return 1;
}

static void
ss_destroy(ProtobufCInputStream *st)
{
	server_streams_destroyed++;
	free(st);
}

static const ProtobufCInputStreamVTable ss_vtable = { ss_send_input, ss_destroy };

static ProtobufCInputStream *
new_server_stream(const Loopback__Req *in, ProtobufCClosure cl, void *d, int echo)
{
	ServerStream *ss = calloc(1, sizeof(*ss));
	ss->base.vtable = &ss_vtable;
	ss->closure = cl;
	ss->closure_data = d;
	ss->echo = echo;
	if (in != NULL)
		ss_send_input(&ss->base, &in->base);
	return &ss->base;
}

static ProtobufCInputStream *
impl_up(Loopback__Counter_Service *s, const Loopback__Req *in,
	Loopback__Rsp_Closure cl, void *d)
{
	(void) s;
	return new_server_stream(in, (ProtobufCClosure) cl, d, 0);
}

static ProtobufCInputStream *
impl_both(Loopback__Counter_Service *s, const Loopback__Req *in,
	  Loopback__Rsp_Closure cl, void *d)
{
	(void) s;
	return new_server_stream(in, (ProtobufCClosure) cl, d, 1);
}

static void
impl_fail(Loopback__Counter_Service *s, const Loopback__Req *in,
	  Loopback__Rsp_Closure cl, void *d)
{
	(void) s; (void) in;
	cl(NULL, d);
}

/* Hold: stash the response closure so it can be called after the call died. */
static ProtobufCClosure held_closure;
static void *held_closure_data;

static ProtobufCInputStream *
impl_hold(Loopback__Counter_Service *s, const Loopback__Req *in,
	  Loopback__Rsp_Closure cl, void *d)
{
	(void) s; (void) in;
	held_closure = (ProtobufCClosure) cl;
	held_closure_data = d;
	return new_server_stream(NULL, (ProtobufCClosure) cl, d, 0);
}

/* Reject: fail immediately, before consuming any of the input stream. */
static ProtobufCInputStream *
impl_reject(Loopback__Counter_Service *s, const Loopback__Req *in,
	    Loopback__Rsp_Closure cl, void *d)
{
	(void) s; (void) in;
	cl(NULL, d);
	return new_server_stream(NULL, (ProtobufCClosure) cl, d, 0);
}

/* ---- pump ---- */

static void
pump(ProtobufCGrpc2Conn *a, ProtobufCGrpc2Conn *b)
{
	uint8_t buf[16384];
	int moved;
	int rounds = 0;

	do {
		ssize_t n;
		moved = 0;
		while ((n = protobuf_c_grpc2_conn_write(a, buf, sizeof buf)) > 0) {
			int rv = protobuf_c_grpc2_conn_read(b, buf, (size_t) n);
			assert(rv == 0);
			moved = 1;
		}
		assert(n == 0);
		while ((n = protobuf_c_grpc2_conn_write(b, buf, sizeof buf)) > 0) {
			int rv = protobuf_c_grpc2_conn_read(a, buf, (size_t) n);
			assert(rv == 0);
			moved = 1;
		}
		assert(n == 0);
		assert(++rounds < 1000);
	} while (moved);
}

/* ---- client side recording ---- */

static int rsp_n[64];
static int rsp_count;
static int rsp_end;
static int last_status = -1;
static char last_message[128];
static int server_status = -1;
static int server_done_count;

static void
reset(void)
{
	rsp_count = rsp_end = 0;
	last_status = -1;
	last_message[0] = 0;
	server_status = -1;
	server_done_count = 0;
}

static void
on_rsp(const Loopback__Rsp *r, void *d)
{
	(void) d;
	if (r == NULL) {
		rsp_end++;
		return;
	}
	rsp_n[rsp_count++] = r->n;
}

static void
on_server_done(const ProtobufCMethodDescriptor *m, int status,
	       const char *message, void *ud)
{
	(void) m; (void) message; (void) ud;
	server_status = status;
	server_done_count++;
}

static void
on_done(const ProtobufCMethodDescriptor *m, int status, const char *message,
	void *ud)
{
	(void) m; (void) ud;
	last_status = status;
	snprintf(last_message, sizeof last_message, "%s", message ? message : "");
}

/* ---- teardown and cancellation ---- */

static void
new_pair(ProtobufCGrpc2Server *server, const ProtobufCGrpc2ConnOptions *copts,
	 const ProtobufCGrpc2ConnOptions *sopts, ProtobufCGrpc2Conn **cc, ProtobufCGrpc2Conn **sc,
	 ProtobufCService **client)
{
	*sc = protobuf_c_grpc2_conn_server_new(server, sopts);
	*cc = protobuf_c_grpc2_conn_client_new(copts);
	assert(*sc != NULL && *cc != NULL);
	*client = protobuf_c_grpc2_conn_client_service(*cc, &loopback__counter__descriptor);
	assert(*client != NULL);
}

static void
check_still_works(ProtobufCGrpc2Conn *cc, ProtobufCGrpc2Conn *sc, ProtobufCService *client)
{
	Loopback__Req req = LOOPBACK__REQ__INIT;

	reset();
	req.n = 1;
	loopback__counter__unary(client, &req, on_rsp, NULL);
	pump(cc, sc);
	assert(rsp_count == 1 && rsp_n[0] == 2 && last_status == PROTOBUF_C_GRPC2_OK);
}

static void
test_teardown_and_cancel(ProtobufCGrpc2Server *server, const ProtobufCGrpc2ConnOptions *copts,
			 const ProtobufCGrpc2ConnOptions *sopts, ProtobufCGrpc2Conn *cc,
			 ProtobufCGrpc2Conn *sc, ProtobufCService *client)
{
	ProtobufCGrpc2Conn *cc2, *sc2;
	ProtobufCService *client2;
	Loopback__Req req = LOOPBACK__REQ__INIT;
	ProtobufCInputStream *in;
	Loopback__Rsp late = LOOPBACK__RSP__INIT;
	int destroyed_before;

	/* 1. client freed with a unary call that was never transmitted */
	new_pair(server, copts, sopts, &cc2, &sc2, &client2);
	reset();
	req.n = 1;
	loopback__counter__unary(client2, &req, on_rsp, NULL);
	protobuf_c_grpc2_conn_free(cc2);
	assert(rsp_count == 0 && rsp_end == 1 && last_status == PROTOBUF_C_GRPC2_UNAVAILABLE);
	protobuf_c_grpc2_conn_free(sc2);
	puts("teardown: client freed, unary in flight ok");

	/* 2. client freed while the application still holds an input stream */
	new_pair(server, copts, sopts, &cc2, &sc2, &client2);
	reset();
	in = loopback__counter__up(client2, NULL, on_rsp, NULL);
	req.n = 5;
	assert(in->vtable->send_input(in, &req.base));
	pump(cc2, sc2);
	protobuf_c_grpc2_conn_free(cc2);
	assert(rsp_end == 1 && last_status == PROTOBUF_C_GRPC2_UNAVAILABLE);
	assert(!in->vtable->send_input(in, &req.base));	/* conn is gone */
	assert(!in->vtable->send_input(in, NULL));
	in->vtable->destroy(in);			/* must be safe */
	destroyed_before = server_streams_destroyed;
	protobuf_c_grpc2_conn_free(sc2);	/* destroys the server's input stream */
	assert(server_streams_destroyed == destroyed_before + 1);
	puts("teardown: client freed, input stream held ok");

	/* 3. server freed with bidirectional calls in flight */
	new_pair(server, copts, sopts, &cc2, &sc2, &client2);
	reset();
	destroyed_before = server_streams_destroyed;
	in = loopback__counter__both(client2, NULL, on_rsp, NULL);
	req.n = 1;
	in->vtable->send_input(in, &req.base);
	pump(cc2, sc2);
	assert(rsp_count == 1);
	protobuf_c_grpc2_conn_free(sc2);
	assert(server_streams_destroyed == destroyed_before + 1);
	assert(server_done_count == 0 || server_status != PROTOBUF_C_GRPC2_OK);
	/* the client notices when it next talks to the dead server; here we
	 * just free it, which completes the call */
	protobuf_c_grpc2_conn_free(cc2);
	assert(rsp_end == 1 && last_status == PROTOBUF_C_GRPC2_UNAVAILABLE);
	in->vtable->destroy(in);
	puts("teardown: server freed, bidi in flight ok");

	/* 4. client cancels by destroying an unfinished input stream */
	reset();
	destroyed_before = server_streams_destroyed;
	in = loopback__counter__both(client, NULL, on_rsp, NULL);
	req.n = 3;
	in->vtable->send_input(in, &req.base);
	pump(cc, sc);
	assert(rsp_count == 1 && rsp_n[0] == 30);
	in->vtable->destroy(in);			/* not ended: cancels */
	pump(cc, sc);
	assert(rsp_end == 1 && last_status == PROTOBUF_C_GRPC2_CANCELLED);
	assert(server_streams_destroyed == destroyed_before + 1);
	assert(server_done_count == 1 && server_status == PROTOBUF_C_GRPC2_CANCELLED);
	check_still_works(cc, sc, client);
	puts("cancel: client destroys unfinished stream ok");

	/* 5. handler answers after its call was cancelled (stale closure) */
	reset();
	held_closure = NULL;
	in = loopback__counter__hold(client, NULL, on_rsp, NULL);
	pump(cc, sc);
	assert(held_closure != NULL);
	in->vtable->destroy(in);
	pump(cc, sc);
	assert(rsp_end == 1 && last_status == PROTOBUF_C_GRPC2_CANCELLED);
	late.n = 99;
	held_closure(&late.base, held_closure_data);	/* ignored */
	held_closure(NULL, held_closure_data);		/* ignored, releases */
	pump(cc, sc);
	assert(rsp_count == 0 && rsp_end == 1);
	check_still_works(cc, sc, client);
	puts("cancel: late handler closure ignored ok");

	/* 6. server rejects before the client has finished sending */
	reset();
	destroyed_before = server_streams_destroyed;
	in = loopback__counter__reject(client, NULL, on_rsp, NULL);
	pump(cc, sc);
	assert(rsp_end == 1 && rsp_count == 0 && last_status == PROTOBUF_C_GRPC2_UNKNOWN);
	req.n = 1;
	in->vtable->send_input(in, &req.base);	/* may or may not be accepted */
	pump(cc, sc);
	assert(!in->vtable->send_input(in, &req.base));
	in->vtable->destroy(in);
	pump(cc, sc);
	assert(server_streams_destroyed == destroyed_before + 1);
	assert(rsp_end == 1);			/* closure not called again */
	check_still_works(cc, sc, client);
	puts("cancel: server rejects early ok");

	/* 7. server connection freed while a handler still holds its closure */
	new_pair(server, copts, sopts, &cc2, &sc2, &client2);
	reset();
	held_closure = NULL;
	in = loopback__counter__hold(client2, NULL, on_rsp, NULL);
	pump(cc2, sc2);
	assert(held_closure != NULL);
	protobuf_c_grpc2_conn_free(sc2);
	late.n = 5;
	held_closure(&late.base, held_closure_data);	/* ignored */
	held_closure(NULL, held_closure_data);		/* final: releases */
	protobuf_c_grpc2_conn_free(cc2);
	in->vtable->destroy(in);
	puts("teardown: stale closure after server freed ok");

	/* 8. cancelling an output-streaming call is by destroying the conn
	 * only; a call that completes normally frees everything */
	reset();
	req.n = 3;
	loopback__counter__down(client, &req, on_rsp, NULL);
	pump(cc, sc);
	assert(rsp_count == 3 && rsp_end == 1);
	puts("teardown tests ok");
}

int
main(void)
{
	Loopback__Counter_Service impl = LOOPBACK__COUNTER__INIT(impl_);
	ProtobufCGrpc2Server *server = protobuf_c_grpc2_server_new();
	ProtobufCGrpc2ConnOptions copts;
	ProtobufCGrpc2ConnOptions sopts;
	ProtobufCGrpc2Conn *cc, *sc;
	ProtobufCService *client;
	Loopback__Req req = LOOPBACK__REQ__INIT;
	ProtobufCInputStream *in;
	int i;

	memset(&copts, 0, sizeof copts);
	memset(&sopts, 0, sizeof sopts);
	copts.on_call_done = on_done;
	sopts.on_call_done = on_server_done;
	assert(server != NULL);
	assert(protobuf_c_grpc2_server_add_service(server, &impl.base) == 0);
	assert(protobuf_c_grpc2_server_add_service(server, &impl.base) == -1);

	sc = protobuf_c_grpc2_conn_server_new(server, &sopts);
	cc = protobuf_c_grpc2_conn_client_new(&copts);
	assert(sc != NULL && cc != NULL);
	client = protobuf_c_grpc2_conn_client_service(cc, &loopback__counter__descriptor);
	assert(client != NULL);

	/* unary */
	reset();
	req.n = 41;
	req.text = "hello";
	loopback__counter__unary(client, &req, on_rsp, NULL);
	pump(cc, sc);
	assert(rsp_count == 1 && rsp_n[0] == 42 && rsp_end == 0);
	assert(last_status == PROTOBUF_C_GRPC2_OK);
	puts("unary ok");

	/* server streaming */
	reset();
	req.n = 5;
	loopback__counter__down(client, &req, on_rsp, NULL);
	pump(cc, sc);
	assert(rsp_count == 5 && rsp_end == 1);
	for (i = 0; i < 5; i++)
		assert(rsp_n[i] == 5 - i);
	assert(last_status == PROTOBUF_C_GRPC2_OK);
	puts("server streaming ok");

	/* client streaming; no first message */
	reset();
	in = loopback__counter__up(client, NULL, on_rsp, NULL);
	assert(in != NULL);
	for (i = 1; i <= 3; i++) {
		req.n = i;
		assert(in->vtable->send_input(in, &req.base));
	}
	pump(cc, sc);
	assert(rsp_count == 0);
	assert(in->vtable->send_input(in, NULL));
	pump(cc, sc);
	assert(rsp_count == 1 && rsp_n[0] == 6);
	assert(last_status == PROTOBUF_C_GRPC2_OK);
	in->vtable->destroy(in);
	puts("client streaming ok");

	/* client streaming; first message given to invoke */
	reset();
	req.n = 100;
	in = loopback__counter__up(client, &req, on_rsp, NULL);
	req.n = 1;
	in->vtable->send_input(in, &req.base);
	in->vtable->send_input(in, NULL);
	pump(cc, sc);
	assert(rsp_count == 1 && rsp_n[0] == 101);
	in->vtable->destroy(in);
	puts("client streaming (first message) ok");

	/* bidirectional: server answers while client is still sending */
	reset();
	in = loopback__counter__both(client, NULL, on_rsp, NULL);
	req.n = 1;
	in->vtable->send_input(in, &req.base);
	pump(cc, sc);
	assert(rsp_count == 1 && rsp_n[0] == 10 && rsp_end == 0);
	req.n = 2;
	in->vtable->send_input(in, &req.base);
	pump(cc, sc);
	assert(rsp_count == 2 && rsp_n[1] == 20 && rsp_end == 0);
	in->vtable->send_input(in, NULL);
	pump(cc, sc);
	assert(rsp_end == 1 && last_status == PROTOBUF_C_GRPC2_OK);
	in->vtable->destroy(in);
	puts("bidirectional ok");

	/* failure: closure gets NULL, status reported out of band */
	reset();
	loopback__counter__fail(client, &req, on_rsp, NULL);
	pump(cc, sc);
	assert(rsp_count == 0 && rsp_end == 1);
	assert(last_status == PROTOBUF_C_GRPC2_UNKNOWN);
	puts("failure ok");

	/* big message (multiple DATA frames and flow control) */
	{
		size_t big = 300000;
		char *text = malloc(big + 1);
		memset(text, 'x', big);
		text[big] = 0;
		reset();
		req.n = 7;
		req.text = text;
		loopback__counter__unary(client, &req, on_rsp, NULL);
		pump(cc, sc);
		assert(rsp_count == 1 && rsp_n[0] == 8 && last_status == PROTOBUF_C_GRPC2_OK);
		free(text);
		req.text = NULL;
	}
	puts("large message ok");

	assert(server_streams_destroyed == 3);

	test_teardown_and_cancel(server, &copts, &sopts, cc, sc, client);

	protobuf_c_grpc2_conn_free(cc);
	protobuf_c_grpc2_conn_free(sc);
	protobuf_c_grpc2_server_free(server);
	puts("loopback: ok");
	return 0;
}
