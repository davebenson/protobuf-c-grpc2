/*
 * Protocol conformance tests, using raw nghttp2 peers.
 *
 * Part 1: a raw nghttp2 *client* sends (mostly malformed) requests to a
 *         ProtobufCGrpc2Conn server and checks the HTTP/gRPC response.
 * Part 2: a raw nghttp2 *server* sends assorted HTTP statuses and
 *         RST_STREAMs to a ProtobufCGrpc2Conn client and checks the
 *         gRPC status the application sees.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nghttp2v2/nghttp2.h>

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"
#include "t/loopback.pb-c.h"

static void
set_nv(nghttp2_nv *nv, const char *name, const char *value)
{
	nv->name = (const uint8_t *) name;
	nv->namelen = strlen(name);
	nv->value = (const uint8_t *) value;
	nv->valuelen = strlen(value);
	nv->flags = NGHTTP2_NV_FLAG_NONE;
}

static nghttp2_tstamp now_ts;

/* ---- the service under test: only Unary is implemented ---- */

static void
impl_unary(Loopback__Counter_Service *s, const Loopback__Req *in,
	   Loopback__Rsp_Closure cl, void *d)
{
	Loopback__Rsp rsp = LOOPBACK__RSP__INIT;
	(void) s;
	rsp.n = in->n + 1;
	cl(&rsp, d);
}

/* ---- raw peer plumbing ---- */

typedef struct {
	int status;			/* ":status", 0 if none */
	int grpc_status;		/* -1 if none */
	char grpc_message[128];
	int headers_fin;		/* the response HEADERS ended the stream */
	int got_trailers;		/* a separate trailers block arrived */
	int headers_blocks;
	int reset;			/* RST_STREAM received */
	uint32_t reset_code;
	/* request to send */
	const char *method, *path, *ctype;
	const uint8_t *body;
	size_t bodylen;
	int sent_body;
	/* for the raw server */
	int mode;
	int closed;
} Raw;

static void
raw_rand(uint8_t *dest, size_t n)
{
	memset(dest, 7, n);
}

static void
raw_clear(Raw *r)
{
	r->status = 0;
	r->grpc_status = -1;
	r->grpc_message[0] = 0;
	r->headers_fin = r->got_trailers = r->headers_blocks = r->reset = 0;
	r->reset_code = 0;
	r->closed = 0;
	r->sent_body = 0;
}

static int
raw_recv_field(nghttp2_conn *c, int64_t id, int32_t token, nghttp2_rcbuf *name,
	       nghttp2_rcbuf *value, uint8_t flags, void *cud, void *sud)
{
	Raw *r = cud;
	nghttp2_vec n = nghttp2_rcbuf_get_buf(name);
	nghttp2_vec v = nghttp2_rcbuf_get_buf(value);
	(void) c; (void) id; (void) token; (void) flags; (void) sud;

	if (n.len == 7 && memcmp(n.base, ":status", 7) == 0) {
		r->status = atoi((const char *) v.base);
	} else if (n.len == 11 && memcmp(n.base, "grpc-status", 11) == 0) {
		r->grpc_status = atoi((const char *) v.base);
	} else if (n.len == 12 && memcmp(n.base, "grpc-message", 12) == 0) {
		snprintf(r->grpc_message, sizeof r->grpc_message, "%.*s",
			 (int) v.len, v.base);
	}
	return 0;
}

static int
raw_begin_headers(nghttp2_conn *c, int64_t id, void *cud, void *sud)
{
	Raw *r = cud;
	(void) c; (void) id; (void) sud;
	r->headers_blocks++;
	return 0;
}

static int
raw_end_headers(nghttp2_conn *c, int64_t id, int fin, void *cud, void *sud)
{
	Raw *r = cud;
	(void) c; (void) id; (void) sud;
	if (fin)
		r->headers_fin = 1;
	return 0;
}

static int
raw_begin_trailers(nghttp2_conn *c, int64_t id, void *cud, void *sud)
{
	Raw *r = cud;
	(void) c; (void) id; (void) sud;
	r->got_trailers = 1;
	return 0;
}

static int
raw_close(nghttp2_conn *c, uint32_t flags, int64_t id, uint32_t code,
	  void *cud, void *sud)
{
	Raw *r = cud;
	(void) c; (void) id; (void) sud;
	r->closed = 1;
	if (flags & NGHTTP2_STREAM_CLOSE_FLAG_ERROR_CODE_SET_REMOTE) {
		r->reset = 1;
		r->reset_code = code;
	}
	return 0;
}

static void
raw_callbacks(nghttp2_callbacks *cb)
{
	memset(cb, 0, sizeof *cb);
	cb->rand = raw_rand;
	cb->begin_headers = raw_begin_headers;
	cb->recv_header = raw_recv_field;
	cb->end_headers = raw_end_headers;
	cb->begin_trailers = raw_begin_trailers;
	cb->recv_trailer = raw_recv_field;
	cb->stream_close = raw_close;
}

/* Move bytes both ways until quiet. */
static void
pump_raw(nghttp2_conn *raw, ProtobufCGrpc2Conn *ours)
{
	uint8_t buf[16384];
	int moved, rounds = 0;

	do {
		nghttp2_ssize n;
		ssize_t m;
		moved = 0;
		while ((n = nghttp2_conn_write(raw, buf, sizeof buf, now_ts += 1000)) > 0) {
			assert(protobuf_c_grpc2_conn_read(ours, buf, (size_t) n) == 0);
			moved = 1;
		}
		assert(n == 0);
		while ((m = protobuf_c_grpc2_conn_write(ours, buf, sizeof buf)) > 0) {
			assert(nghttp2_conn_read(raw, buf, (size_t) m, now_ts += 1000) == 0);
			moved = 1;
		}
		assert(m == 0);
		assert(++rounds < 100);
	} while (moved);
}

/* ---- part 1: raw client -> our server ---- */

static nghttp2_ssize
body_reader(nghttp2_conn *c, int64_t id, nghttp2_vec *vec, size_t cnt,
	    uint32_t *flags, void *cud, void *sud)
{
	Raw *r = cud;
	(void) c; (void) id; (void) cnt; (void) sud;
	if (r->sent_body) {
		*flags |= NGHTTP2_READ_DATA_FLAG_EOF;
		return 0;
	}
	r->sent_body = 1;
	vec[0].base = (uint8_t *) r->body;
	vec[0].len = r->bodylen;
	*flags |= NGHTTP2_READ_DATA_FLAG_EOF;
	return 1;
}

static void
raw_request(ProtobufCGrpc2Server *server, Raw *r)
{
	nghttp2_conn *raw;
	nghttp2_callbacks cb;
	nghttp2_settings st;
	ProtobufCGrpc2Conn *ours;
	nghttp2_nv nva[8];
	size_t n = 0;
	nghttp2_data_reader dr;

	raw_clear(r);
	raw_callbacks(&cb);
	nghttp2_settings_default(&st);
	assert(nghttp2_conn_client_new(&raw, &cb, &st, NULL, r) == 0);
	ours = protobuf_c_grpc2_conn_server_new(server, NULL);

#define NV(nm, val) do { \
	nva[n].name = (const uint8_t *) (nm); nva[n].namelen = strlen(nm); \
	nva[n].value = (const uint8_t *) (val); nva[n].valuelen = strlen(val); \
	nva[n].flags = NGHTTP2_NV_FLAG_NONE; n++; } while (0)
	NV(":method", r->method);
	NV(":scheme", "http");
	NV(":path", r->path);
	NV(":authority", "localhost");
	if (r->ctype != NULL)
		NV("content-type", r->ctype);
	NV("te", "trailers");
#undef NV
	dr.read_data = body_reader;
	assert(nghttp2_conn_submit_request(raw, nva, n,
					   r->bodylen ? &dr : NULL, NULL) >= 0);
	pump_raw(raw, ours);
	assert(r->closed);
	nghttp2_conn_del(raw);
	protobuf_c_grpc2_conn_free(ours);
}

/* a gRPC message frame around `payload` (a serialized Req) */
static size_t
frame(uint8_t *out, uint8_t compressed, const uint8_t *payload, size_t n)
{
	out[0] = compressed;
	out[1] = (uint8_t) (n >> 24);
	out[2] = (uint8_t) (n >> 16);
	out[3] = (uint8_t) (n >> 8);
	out[4] = (uint8_t) n;
	memcpy(out + 5, payload, n);
	return 5 + n;
}

static void
test_server_responses(void)
{
	Loopback__Counter_Service impl;
	ProtobufCGrpc2Server *server = protobuf_c_grpc2_server_new();
	Raw r;
	uint8_t body[256], payload[64];
	size_t n, pn;
	Loopback__Req req = LOOPBACK__REQ__INIT;

	loopback__counter__init(&impl, NULL);
	impl.unary = impl_unary;
	assert(protobuf_c_grpc2_server_add_service(server, &impl.base) == 0);
	memset(&r, 0, sizeof r);
	req.n = 1;
	pn = protobuf_c_message_pack(&req.base, payload);

	/* sanity: a valid unary request works */
	r.method = "POST"; r.path = "/loopback.Counter/Unary";
	r.ctype = "application/grpc+proto";
	n = frame(body, 0, payload, pn);
	r.body = body; r.bodylen = n;
	raw_request(server, &r);
	assert(r.status == 200 && r.grpc_status == 0 && r.got_trailers);
	assert(!r.headers_fin);
	puts("protocol: valid request ok");

	/* unknown service: Trailers-Only (one HEADERS block that ends the stream) */
	r.path = "/loopback.Nope/Unary";
	raw_request(server, &r);
	assert(r.status == 200 && r.grpc_status == PROTOBUF_C_GRPC2_UNIMPLEMENTED);
	assert(r.headers_fin && !r.got_trailers && r.headers_blocks == 1);
	puts("protocol: unknown service is a Trailers-Only UNIMPLEMENTED ok");

	/* unknown method */
	r.path = "/loopback.Counter/Nope";
	raw_request(server, &r);
	assert(r.grpc_status == PROTOBUF_C_GRPC2_UNIMPLEMENTED && r.headers_fin);

	/* a path with no method */
	r.path = "/loopback.Counter";
	raw_request(server, &r);
	assert(r.grpc_status == PROTOBUF_C_GRPC2_UNIMPLEMENTED && r.headers_fin);
	puts("protocol: unknown method / bad path ok");

	/* not gRPC: wrong or missing content-type -> HTTP 415, wrong method -> 405 */
	r.path = "/loopback.Counter/Unary";
	r.ctype = "text/plain";
	raw_request(server, &r);
	assert(r.status == 415 && r.grpc_status == -1 && r.headers_fin);
	r.ctype = NULL;
	raw_request(server, &r);
	assert(r.status == 415 && r.headers_fin);
	r.ctype = "application/grpc";
	r.method = "GET";
	raw_request(server, &r);
	assert(r.status == 405 && r.headers_fin);
	r.method = "POST";
	puts("protocol: HTTP 415 / 405 for non-gRPC requests ok");

	/* request cardinality: two messages for a unary call -> UNIMPLEMENTED */
	n = frame(body, 0, payload, pn);
	n += frame(body + n, 0, payload, pn);
	r.body = body; r.bodylen = n;
	raw_request(server, &r);
	assert(r.grpc_status == PROTOBUF_C_GRPC2_UNIMPLEMENTED);
	/* no message at all (empty body) -> UNIMPLEMENTED */
	r.body = NULL; r.bodylen = 0;
	raw_request(server, &r);
	assert(r.grpc_status == PROTOBUF_C_GRPC2_UNIMPLEMENTED);
	puts("protocol: request cardinality violations ok");

	/* a message that does not parse -> INTERNAL (not INVALID_ARGUMENT) */
	{
		static const uint8_t junk[] = { 0x0a, 0xff, 0xff, 0xff };
		n = frame(body, 0, junk, sizeof junk);
		r.body = body; r.bodylen = n;
		raw_request(server, &r);
		assert(r.grpc_status == PROTOBUF_C_GRPC2_INTERNAL);
	}
	/* compressed message with no encoding negotiated -> UNIMPLEMENTED */
	n = frame(body, 1, payload, pn);
	r.body = body; r.bodylen = n;
	raw_request(server, &r);
	assert(r.grpc_status == PROTOBUF_C_GRPC2_UNIMPLEMENTED);
	/* truncated message (length says more than is sent) -> INTERNAL */
	n = frame(body, 0, payload, pn);
	r.body = body; r.bodylen = n - 1;
	raw_request(server, &r);
	assert(r.grpc_status == PROTOBUF_C_GRPC2_INTERNAL);
	puts("protocol: bad message payloads ok");

	protobuf_c_grpc2_server_free(server);
}

/* ---- part 2: our client -> raw server ---- */

enum { M_HTTP_STATUS = 1, M_RST, M_OK_NO_STATUS };

static int srv_http_status;
static uint32_t srv_rst_code;

static int
fake_end_headers(nghttp2_conn *c, int64_t id, int fin, void *cud, void *sud)
{
	Raw *r = cud;
	(void) fin; (void) sud;

	if (r->mode == M_RST) {
		nghttp2_conn_shutdown_stream(c, 0, id, srv_rst_code);
	} else if (r->mode == M_HTTP_STATUS) {
		char code[16];
		nghttp2_nv nv[1];
		snprintf(code, sizeof code, "%d", srv_http_status);
		set_nv(&nv[0], ":status", code);
		/* headers only: ends the stream */
		assert(nghttp2_conn_submit_response(c, id, nv, 1, NULL) == 0);
	} else {	/* M_OK_NO_STATUS: 200 and a clean end with no grpc-status */
		nghttp2_nv nv[2];
		set_nv(&nv[0], ":status", "200");
		set_nv(&nv[1], "content-type", "application/grpc");
		assert(nghttp2_conn_submit_response(c, id, nv, 2, NULL) == 0);
	}
	return 0;
}

static int last_status;
static char last_msg[128];
static int got_reply;

static void
cb_done(const ProtobufCMethodDescriptor *m, int status, const char *msg, void *ud)
{
	(void) m; (void) ud;
	last_status = status;
	snprintf(last_msg, sizeof last_msg, "%s", msg ? msg : "");
}

static void
cb_rsp(const Loopback__Rsp *r, void *d)
{
	(void) d;
	if (r != NULL)
		got_reply++;
}

static int
client_sees(int mode, int http_status, uint32_t rst_code)
{
	nghttp2_conn *raw;
	nghttp2_callbacks cb;
	nghttp2_settings st;
	ProtobufCGrpc2Conn *ours;
	ProtobufCGrpc2ConnOptions o;
	ProtobufCService *svc;
	Loopback__Req req = LOOPBACK__REQ__INIT;
	Raw r;

	memset(&r, 0, sizeof r);
	r.mode = mode;
	srv_http_status = http_status;
	srv_rst_code = rst_code;
	raw_callbacks(&cb);
	cb.end_headers = fake_end_headers;
	nghttp2_settings_default(&st);
	st.max_concurrent_streams_remote = 10;
	assert(nghttp2_conn_server_new(&raw, &cb, &st, NULL, &r) == 0);
	memset(&o, 0, sizeof o);
	o.on_call_done = cb_done;
	ours = protobuf_c_grpc2_conn_client_new(&o);
	svc = protobuf_c_grpc2_conn_client_service(ours, &loopback__counter__descriptor);

	last_status = -1;
	last_msg[0] = 0;
	got_reply = 0;
	req.n = 1;
	loopback__counter__unary(svc, &req, cb_rsp, NULL);
	pump_raw(raw, ours);
	assert(got_reply == 0);
	nghttp2_conn_del(raw);
	protobuf_c_grpc2_conn_free(ours);
	return last_status;
}

static void
test_client_mapping(void)
{
	/* non-200 HTTP statuses without grpc-status */
	assert(client_sees(M_HTTP_STATUS, 400, 0) == PROTOBUF_C_GRPC2_INTERNAL);
	assert(client_sees(M_HTTP_STATUS, 401, 0) == PROTOBUF_C_GRPC2_UNAUTHENTICATED);
	assert(client_sees(M_HTTP_STATUS, 403, 0) == PROTOBUF_C_GRPC2_PERMISSION_DENIED);
	assert(client_sees(M_HTTP_STATUS, 404, 0) == PROTOBUF_C_GRPC2_UNIMPLEMENTED);
	assert(client_sees(M_HTTP_STATUS, 429, 0) == PROTOBUF_C_GRPC2_UNAVAILABLE);
	assert(client_sees(M_HTTP_STATUS, 503, 0) == PROTOBUF_C_GRPC2_UNAVAILABLE);
	assert(client_sees(M_HTTP_STATUS, 418, 0) == PROTOBUF_C_GRPC2_UNKNOWN);
	assert(strcmp(last_msg, "HTTP 418") == 0);
	puts("protocol: HTTP status mapping ok");

	/* 200 and a clean close but no grpc-status */
	assert(client_sees(M_OK_NO_STATUS, 0, 0) == PROTOBUF_C_GRPC2_INTERNAL);

	/* RST_STREAM codes sent by the server */
	assert(client_sees(M_RST, 0, 7) == PROTOBUF_C_GRPC2_UNAVAILABLE);	/* REFUSED_STREAM */
	assert(client_sees(M_RST, 0, 8) == PROTOBUF_C_GRPC2_CANCELLED);		/* CANCEL */
	assert(client_sees(M_RST, 0, 11) == PROTOBUF_C_GRPC2_RESOURCE_EXHAUSTED);
	assert(client_sees(M_RST, 0, 12) == PROTOBUF_C_GRPC2_PERMISSION_DENIED);
	assert(client_sees(M_RST, 0, 2) == PROTOBUF_C_GRPC2_INTERNAL);		/* INTERNAL_ERROR */
	assert(client_sees(M_RST, 0, 1) == PROTOBUF_C_GRPC2_INTERNAL);		/* PROTOCOL_ERROR */
	puts("protocol: RST_STREAM mapping ok");
}

int
main(void)
{
	test_server_responses();
	test_client_mapping();
	puts("protocol: ok");
	return 0;
}
