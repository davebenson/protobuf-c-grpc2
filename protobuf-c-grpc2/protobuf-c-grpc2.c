/*
 * protobuf-c-grpc2: gRPC over HTTP/2 for protobuf-c, built on nghttp2-v2.
 * See protobuf-c-grpc2.h for the public API.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nghttp2v2/nghttp2.h>

#include "protobuf-c-grpc2-internal.h"

#define DEFAULT_MAX_MESSAGE_SIZE ((size_t) 4 * 1024 * 1024)
#define FRAME_HEADER_LEN 5
#define HTTP2_CANCEL 8u

typedef struct Call Call;
typedef struct ClientService ClientService;

struct ProtobufCGrpc2Conn {
	nghttp2_conn *ng;
	int is_server;
	ProtobufCGrpc2Server *server;
	ProtobufCGrpc2ConnOptions options;
	char *authority;
	char *scheme;
	size_t max_message_size;
	char *peer;			/* TLS peer identity, or NULL */
	Call *calls;			/* all live calls, doubly linked */
	ClientService *client_services;
};

struct ClientService {
	ProtobufCService base;		/* must be first */
	uint64_t timeout_ms;		/* 0: calls have no deadline */
	ProtobufCGrpc2Client *client;	/* channel mode: the connection comes
					 * from here (it may change) */
	ProtobufCGrpc2Conn *conn;	/* direct mode */
	ClientService *next;
};

/* Call::auth_state: the authenticator's verdict for a server call */
enum {
	AUTH_NONE,		/* no authenticator, or not asked yet */
	AUTH_CHECKING,		/* inside check(): a reply now is the fast path */
	AUTH_REPLIED,		/* the reply came before check() returned */
	AUTH_PENDING,		/* check() returned, the reply is awaited */
	AUTH_DONE
};

/* Call::cred_state: the same for the credentials of a client call */
enum {
	CRED_NONE,
	CRED_CHECKING,
	CRED_PENDING,
	CRED_DONE
};

/* The request side of a client call, handed to the application. */
typedef struct {
	ProtobufCInputStream base;
	Call *call;
} ClientSink;

/*
 * One RPC. Reference counted: one reference is held by the nghttp2 stream
 * (dropped at stream close or connection free), one by a ClientSink, and one
 * by the server-side response closure until the handler has had its last
 * chance to call it.
 */
struct Call {
	ProtobufCGrpc2Conn *conn;		/* NULL once the connection is gone */
	Call *prev, *next;
	int refs;
	int is_server;
	int64_t stream_id;

	/* method being invoked */
	ProtobufCService *service;	/* server only */
	const ProtobufCMethodDescriptor *method;
	unsigned method_index;
	unsigned stream_flags;		/* PROTOBUF_C_*_STREAMING of the method */

	/* outgoing body: `pend` is being filled, `infl` is owned by nghttp2 */
	uint8_t *pend;
	size_t pend_len, pend_cap;
	uint8_t *infl;
	size_t infl_len, infl_written;
	int out_finished;		/* no more data after pend */

	/* incoming body: message framing state */
	uint8_t fhdr[FRAME_HEADER_LEN];
	size_t fhdr_got;
	uint8_t *msg;
	size_t msg_len, msg_got;
	int in_msg;

	/* flags */
	uint64_t deadline_ns;		/* absolute (now_ns() clock); 0 = none */
	int has_timeout;		/* server: a valid grpc-timeout was sent */
	uint64_t timeout_ns;		/* server: its value */
	int stream_gone;		/* nghttp2 stream closed */
	int remote_ended;		/* peer finished sending */
	int done;			/* final status determined */

	/* server side */
	char *path;
	int http_error;			/* HTTP status to reject the request with */
	int saw_content_type;
	int replied;			/* response headers have been submitted */
	int headers_done;		/* request headers processed */
	int dispatched;
	int got_request;
	ProtobufCMessage *request;	/* held until the request half-closes */
	int closure_ref;		/* holds a reference for the closure */
	ProtobufCInputStream *sink;	/* input stream from the handler */
	int in_sink_send;		/* inside sink->send(msg) */
	int sink_end_pending;		/* end it when that returns */

	/* server: authentication (see server_auth_begin) */
	char **auth_names, **auth_values;	/* requested headers received */
	size_t n_auth_headers;
	int auth_state;			/* AUTH_* */
	int auth_status;		/* the verdict, when it came in the hook */
	char *auth_message;
	char *principal;		/* the identity the authenticator gave */
	uint8_t *held;			/* request data received while waiting */
	size_t held_len, held_cap;
	int headers_fin;		/* the request headers ended the stream */

	/* client: what is needed to send the request once the credentials
	 * are in (see client_submit) */
	char *req_path;
	uint64_t req_timeout_ms;
	int cred_state;			/* CRED_* */

	/* client side */
	ProtobufCClosure closure;
	void *closure_data;
	int delivered;			/* a response message was delivered */
	int http_status;
	int grpc_status;		/* -1 until seen */
	char *grpc_message;
};

/* ------------------------------------------------------------------ */
/* utilities */

static const char *const status_names[] = {
	"OK", "CANCELLED", "UNKNOWN", "INVALID_ARGUMENT", "DEADLINE_EXCEEDED",
	"NOT_FOUND", "ALREADY_EXISTS", "PERMISSION_DENIED",
	"RESOURCE_EXHAUSTED", "FAILED_PRECONDITION", "ABORTED", "OUT_OF_RANGE",
	"UNIMPLEMENTED", "INTERNAL", "UNAVAILABLE", "DATA_LOSS",
	"UNAUTHENTICATED"
};

const char *
protobuf_c_grpc2_status_name(int status)
{
	if (status < 0 || (size_t) status >= sizeof(status_names) / sizeof(status_names[0]))
		return "UNKNOWN";
	return status_names[status];
}

static char *
xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *r = malloc(n);
	if (r != NULL)
		memcpy(r, s, n);
	return r;
}

static char *
xstrndup(const uint8_t *s, size_t n)
{
	char *r = malloc(n + 1);
	if (r != NULL) {
		memcpy(r, s, n);
		r[n] = 0;
	}
	return r;
}

static nghttp2_tstamp
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (nghttp2_tstamp) ts.tv_sec * 1000000000u + (nghttp2_tstamp) ts.tv_nsec;
}

static void
rand_cb(uint8_t *dest, size_t destlen)
{
	FILE *f = fopen("/dev/urandom", "rb");
	size_t got = 0;
	if (f != NULL) {
		got = fread(dest, 1, destlen, f);
		fclose(f);
	}
	/* Only used for PING payloads, padding etc: not security critical. */
	for (; got < destlen; got++)
		dest[got] = (uint8_t) rand();
}

/* grpc-message percent encoding (gRPC over HTTP/2 spec). */
static char *
pct_encode(const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = strlen(s), i, o = 0;
	char *r = malloc(n * 3 + 1);
	if (r == NULL)
		return NULL;
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char) s[i];
		if (c >= 0x20 && c <= 0x7e && c != '%') {
			r[o++] = (char) c;
		} else {
			r[o++] = '%';
			r[o++] = hex[c >> 4];
			r[o++] = hex[c & 15];
		}
	}
	r[o] = 0;
	return r;
}

static int
hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static char *
pct_decode(const uint8_t *s, size_t n)
{
	size_t i, o = 0;
	char *r = malloc(n + 1);
	if (r == NULL)
		return NULL;
	for (i = 0; i < n; i++) {
		if (s[i] == '%' && i + 2 < n &&
		    hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
			r[o++] = (char) (hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
			i += 2;
		} else {
			r[o++] = (char) s[i];
		}
	}
	r[o] = 0;
	return r;
}

/*
 * grpc-timeout: 1 to 8 ASCII digits and a unit (H M S m u n). Returns 0 and
 * the value in nanoseconds (saturated), or -1 if malformed.
 */
static int
parse_grpc_timeout(const uint8_t *v, size_t len, uint64_t *ns)
{
	static const uint64_t HUGE_NS = (uint64_t) 1 << 62;
	uint64_t n = 0, unit;
	size_t i;

	if (len < 2 || len > 9)
		return -1;
	for (i = 0; i + 1 < len; i++) {
		if (v[i] < '0' || v[i] > '9')
			return -1;
		n = n * 10 + (uint64_t) (v[i] - '0');
	}
	switch (v[len - 1]) {
	case 'H': unit = (uint64_t) 3600 * 1000000000u; break;
	case 'M': unit = (uint64_t) 60 * 1000000000u; break;
	case 'S': unit = 1000000000u; break;
	case 'm': unit = 1000000u; break;
	case 'u': unit = 1000u; break;
	case 'n': unit = 1; break;
	default: return -1;
	}
	*ns = (n > HUGE_NS / unit) ? HUGE_NS : n * unit;
	return 0;
}

/* Encode a timeout in ms as a grpc-timeout value (at most 8 digits). */
static void
format_grpc_timeout(uint64_t ms, char out[16])
{
	const uint64_t MAXV = 99999999u;

	if (ms <= MAXV) {
		snprintf(out, 16, "%um", (unsigned) ms);
		return;
	}
	ms = (ms + 999) / 1000;			/* seconds, rounded up */
	if (ms <= MAXV) {
		snprintf(out, 16, "%uS", (unsigned) ms);
		return;
	}
	ms = (ms + 59) / 60;
	if (ms <= MAXV) {
		snprintf(out, 16, "%uM", (unsigned) ms);
		return;
	}
	ms = (ms + 59) / 60;
	snprintf(out, 16, "%uH", (unsigned) (ms > MAXV ? MAXV : ms));
}

static uint64_t
add_sat(uint64_t a, uint64_t b)
{
	return (a + b < a) ? UINT64_MAX : a + b;
}

static nghttp2_nv
nv_lit(const char *name, const char *value)
{
	nghttp2_nv nv;
	nv.name = (const uint8_t *) name;
	nv.namelen = strlen(name);
	nv.value = (const uint8_t *) value;
	nv.valuelen = strlen(value);
	nv.flags = NGHTTP2_NV_FLAG_NO_COPY_NAME;
	return nv;
}

/* ------------------------------------------------------------------ */
/* call lifetime */

static Call *
call_new(ProtobufCGrpc2Conn *conn, int is_server)
{
	Call *c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->conn = conn;
	c->is_server = is_server;
	c->refs = 1;			/* the nghttp2 stream's reference */
	c->grpc_status = -1;
	c->stream_id = -1;
	c->next = conn->calls;
	if (conn->calls != NULL)
		conn->calls->prev = c;
	conn->calls = c;
	return c;
}

static void
call_unlink(Call *c)
{
	if (c->conn == NULL)
		return;
	if (c->prev != NULL)
		c->prev->next = c->next;
	else
		c->conn->calls = c->next;
	if (c->next != NULL)
		c->next->prev = c->prev;
	c->prev = c->next = NULL;
	c->conn = NULL;
}

static void
call_unref(Call *c)
{
	if (--c->refs > 0)
		return;
	call_unlink(c);
	free(c->pend);
	free(c->infl);
	free(c->msg);
	free(c->path);
	free(c->grpc_message);
	{
		size_t i;

		for (i = 0; i < c->n_auth_headers; i++) {
			free(c->auth_names[i]);
			free(c->auth_values[i]);
		}
		free(c->auth_names);
		free(c->auth_values);
	}
	free(c->auth_message);
	free(c->principal);
	free(c->held);
	free(c->req_path);
	if (c->request != NULL)
		protobuf_c_message_free_unpacked(c->request, NULL);
	free(c);
}

static int
call_pend_reserve(Call *c, size_t extra)
{
	size_t need = c->pend_len + extra;
	if (need > c->pend_cap) {
		size_t cap = c->pend_cap ? c->pend_cap : 256;
		uint8_t *p;
		while (cap < need)
			cap *= 2;
		p = realloc(c->pend, cap);
		if (p == NULL)
			return -1;
		c->pend = p;
		c->pend_cap = cap;
	}
	return 0;
}

/* Frame `msg` (gRPC length-prefixed) and queue it. */
static int
call_queue_message(Call *c, const ProtobufCMessage *msg)
{
	size_t size = protobuf_c_message_get_packed_size(msg);
	uint8_t *p;

	if (c->conn == NULL || c->out_finished)
		return -1;
	if (size > c->conn->max_message_size || size > 0xffffffffu)
		return -1;
	if (call_pend_reserve(c, FRAME_HEADER_LEN + size) != 0)
		return -1;
	p = c->pend + c->pend_len;
	p[0] = 0;
	p[1] = (uint8_t) (size >> 24);
	p[2] = (uint8_t) (size >> 16);
	p[3] = (uint8_t) (size >> 8);
	p[4] = (uint8_t) size;
	protobuf_c_message_pack(msg, p + FRAME_HEADER_LEN);
	c->pend_len += FRAME_HEADER_LEN + size;
	if (c->stream_id >= 0)		/* else not submitted yet: it is sent then */
		nghttp2_conn_resume_stream(c->conn->ng, c->stream_id);
	return 0;
}

static void
call_finish_output(Call *c)
{
	if (c->out_finished)
		return;
	c->out_finished = 1;
	if (c->conn != NULL && !c->stream_gone && c->stream_id >= 0)
		nghttp2_conn_resume_stream(c->conn->ng, c->stream_id);
}

/* nghttp2 body reader shared by both ends. */
static nghttp2_ssize
read_data_cb(nghttp2_conn *ng, int64_t stream_id, nghttp2_vec *vec,
	     size_t veccnt, uint32_t *pflags, void *conn_ud, void *stream_ud)
{
	Call *c = stream_ud;
	(void) ng; (void) stream_id; (void) veccnt; (void) conn_ud;

	if (c == NULL) {
		*pflags |= NGHTTP2_READ_DATA_FLAG_EOF;
		return 0;
	}
	if (c->pend_len > 0 && c->infl == NULL) {
		c->infl = c->pend;
		c->infl_len = c->pend_len;
		c->infl_written = 0;
		c->pend = NULL;
		c->pend_len = c->pend_cap = 0;
		vec[0].base = c->infl;
		vec[0].len = c->infl_len;
		return 1;
	}
	if (c->pend_len == 0 && c->out_finished) {
		*pflags |= NGHTTP2_READ_DATA_FLAG_EOF;
		return 0;
	}
	return NGHTTP2_ERR_WOULDBLOCK;
}

static int
write_stream_data_offset_cb(nghttp2_conn *ng, int64_t stream_id,
			    uint64_t offset, size_t len, void *conn_ud,
			    void *stream_ud)
{
	Call *c = stream_ud;
	(void) ng; (void) stream_id; (void) offset; (void) conn_ud;

	if (c == NULL || c->infl == NULL)
		return 0;
	c->infl_written += len;
	if (c->infl_written >= c->infl_len) {
		free(c->infl);
		c->infl = NULL;
		c->infl_len = c->infl_written = 0;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* message framing (both directions) */

typedef int (*MessageFn)(Call *c, const uint8_t *data, size_t len);

/*
 * Feed body bytes; calls `fn` for each complete message. Returns 0 or a
 * gRPC status code describing a framing problem.
 */
static int
call_feed(Call *c, const uint8_t *data, size_t len, MessageFn fn)
{
	while (len > 0) {
		if (!c->in_msg) {
			size_t n = FRAME_HEADER_LEN - c->fhdr_got;
			if (n > len)
				n = len;
			memcpy(c->fhdr + c->fhdr_got, data, n);
			c->fhdr_got += n;
			data += n;
			len -= n;
			if (c->fhdr_got < FRAME_HEADER_LEN)
				break;
			c->fhdr_got = 0;
			if (c->fhdr[0] != 0)
				return PROTOBUF_C_GRPC2_UNIMPLEMENTED; /* compression */
			c->msg_len = ((size_t) c->fhdr[1] << 24) |
				     ((size_t) c->fhdr[2] << 16) |
				     ((size_t) c->fhdr[3] << 8) | c->fhdr[4];
			if (c->msg_len > c->conn->max_message_size)
				return PROTOBUF_C_GRPC2_RESOURCE_EXHAUSTED;
			c->msg_got = 0;
			c->in_msg = 1;
			free(c->msg);
			c->msg = malloc(c->msg_len ? c->msg_len : 1);
			if (c->msg == NULL)
				return PROTOBUF_C_GRPC2_RESOURCE_EXHAUSTED;
		}
		{
			size_t n = c->msg_len - c->msg_got;
			int rv;
			if (n > len)
				n = len;
			memcpy(c->msg + c->msg_got, data, n);
			c->msg_got += n;
			data += n;
			len -= n;
			if (c->msg_got < c->msg_len)
				break;
			c->in_msg = 0;
			rv = fn(c, c->msg, c->msg_len);
			free(c->msg);
			c->msg = NULL;
			if (rv != 0)
				return rv;
			if (c->conn == NULL)
				return 0;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* server side */

static void
server_reply_headers(Call *c)
{
	nghttp2_nv nva[2];
	nghttp2_data_reader dr;

	nva[0] = nv_lit(":status", "200");
	nva[1] = nv_lit("content-type", "application/grpc");
	dr.read_data = read_data_cb;
	nghttp2_conn_submit_response(c->conn->ng, c->stream_id, nva, 2, &dr);
	c->replied = 1;
}

static void
call_report_done(Call *c, int status, const char *message)
{
	if (c->conn != NULL && c->conn->options.on_call_done != NULL)
		c->conn->options.on_call_done(c->method, status, message,
					      c->conn->options.user_data);
}

static void
server_release_closure(Call *c)
{
	if (c->closure_ref) {
		c->closure_ref = 0;
		call_unref(c);
	}
}

/* Reject a request that is not gRPC at all with a plain HTTP status. */
static void
server_http_error(Call *c, int http_status)
{
	nghttp2_nv nva[1];
	char code[16];
	char text[32];

	if (c->done || c->conn == NULL)
		return;
	c->done = 1;
	snprintf(code, sizeof code, "%d", http_status);
	nva[0] = nv_lit(":status", code);
	snprintf(text, sizeof text, "HTTP %d", http_status);
	call_report_done(c, PROTOBUF_C_GRPC2_UNKNOWN, text);
	c->replied = 1;
	c->out_finished = 1;
	if (!c->stream_gone)
		nghttp2_conn_submit_response(c->conn->ng, c->stream_id, nva, 1,
					     NULL);
}

/* Tell the handler's input stream that its input is over (normally, or
 * because the call is). Ending releases the stream. From inside the stream's
 * own send(msg) the end is deferred until that returns, so that a handler
 * that responds to a message never has its stream ended under its feet. */
static void
server_end_sink(Call *c)
{
	ProtobufCInputStream *s = c->sink;

	if (s == NULL)
		return;
	if (c->in_sink_send) {
		c->sink_end_pending = 1;
		return;
	}
	c->sink = NULL;
	s->send(s, NULL);
}

static void
server_finish(Call *c, int status, const char *message)
{
	nghttp2_nv nva[4];
	char code[16];
	char *enc = NULL;
	size_t n = 1;

	if (c->done || c->conn == NULL)
		return;
	c->done = 1;
	server_end_sink(c);
	if (c->stream_gone)
		return;

	snprintf(code, sizeof code, "%d", status);
	if (!c->replied) {
		/* Trailers-Only: status and trailers in the one HEADERS frame
		 * that also ends the stream (for errors found before any
		 * response was started). */
		nva[0] = nv_lit(":status", "200");
		nva[1] = nv_lit("content-type", "application/grpc");
		nva[2] = nv_lit("grpc-status", code);
		n = 3;
		if (message != NULL && message[0] != 0 &&
		    (enc = pct_encode(message)) != NULL) {
			nva[3] = nv_lit("grpc-message", enc);
			n = 4;
		}
		call_report_done(c, status, message);
		c->replied = 1;
		nghttp2_conn_submit_response(c->conn->ng, c->stream_id, nva, n,
					     NULL);
		free(enc);
		c->out_finished = 1;
		return;
	}
	nva[0] = nv_lit("grpc-status", code);
	n = 1;
	if (message != NULL && message[0] != 0) {
		enc = pct_encode(message);
		if (enc != NULL) {
			nva[1] = nv_lit("grpc-message", enc);
			n = 2;
		}
	}
	call_report_done(c, status, message);
	nghttp2_conn_submit_trailers(c->conn->ng, c->stream_id, nva, n);
	free(enc);
	call_finish_output(c);
}

/* The response closure given to the service implementation. */
static void
server_closure(const ProtobufCMessage *msg, void *data)
{
	Call *c = data;

	if (msg == NULL) {
		/* End of an output stream; for unary this signals failure. */
		if (c->stream_flags & PROTOBUF_C_OUTPUT_STREAMING)
			server_finish(c, PROTOBUF_C_GRPC2_OK, NULL);
		else
			server_finish(c, PROTOBUF_C_GRPC2_UNKNOWN, "method failed");
		server_release_closure(c);
		return;
	}
	if (c->done || c->stream_gone || c->conn == NULL)
		return;
	if (call_queue_message(c, msg) != 0) {
		server_finish(c, PROTOBUF_C_GRPC2_INTERNAL, "cannot send response");
		server_release_closure(c);
		return;
	}
	if (!(c->stream_flags & PROTOBUF_C_OUTPUT_STREAMING)) {
		server_finish(c, PROTOBUF_C_GRPC2_OK, NULL);
		server_release_closure(c);
	}
}

void
protobuf_c_grpc2__server_respond(Call *c, const ProtobufCMessage *msg)
{
	server_closure(msg, c);
}

static _Thread_local const char *current_principal;

const char *
protobuf_c_grpc2_current_call_principal(void)
{
	return current_principal;
}

static void
server_invoke(Call *c, const ProtobufCMessage *input)
{
	const char *saved_principal = current_principal;
	ProtobufCClosure closure = (ProtobufCClosure) server_closure;
	void *closure_data = c;

	c->closure_ref = 1;
	c->refs++;
	if (c->conn != NULL && c->conn->server != NULL &&
	    c->conn->server->shared != NULL) {
		/* thread_safe: the handler gets a closure that may be called
		 * from any thread; it is freed by the final call */
		void *safe = protobuf_c_grpc2__safe_closure_new(
			c->conn->server->shared, c, c->method->output,
			(c->stream_flags & PROTOBUF_C_OUTPUT_STREAMING) != 0);

		if (safe != NULL) {
			closure = (ProtobufCClosure) protobuf_c_grpc2__safe_closure;
			closure_data = safe;
		}
	}
	current_principal = c->principal;
	c->sink = c->service->invoke(c->service,
				     c->method_index | c->stream_flags, input,
				     closure, closure_data);
	current_principal = saved_principal;
}

/* A complete request message arrived. */
static int
server_on_message(Call *c, const uint8_t *data, size_t len)
{
	ProtobufCMessage *msg;

	if (c->done)
		return 0;
	msg = protobuf_c_message_unpack(c->method->input, NULL, len, data);
	if (msg == NULL)
		return PROTOBUF_C_GRPC2_INTERNAL;	/* request parse failure */
	if (c->stream_flags & PROTOBUF_C_INPUT_STREAMING) {
		if (c->sink != NULL) {
			protobuf_c_boolean ok;

			c->in_sink_send++;
			ok = c->sink->send(c->sink, msg);
			c->in_sink_send--;
			if (c->in_sink_send == 0 && c->sink_end_pending) {
				c->sink_end_pending = 0;
				server_end_sink(c);
			}
			if (!ok) {
				protobuf_c_message_free_unpacked(msg, NULL);
				return PROTOBUF_C_GRPC2_INTERNAL;
			}
		}
	} else if (c->got_request) {
		/* request cardinality violation */
		protobuf_c_message_free_unpacked(msg, NULL);
		return PROTOBUF_C_GRPC2_UNIMPLEMENTED;
	} else {
		/* Keep the request; the handler runs when the client has
		 * half-closed, so that extra messages can be detected. */
		c->got_request = 1;
		c->request = msg;
		return 0;
	}
	protobuf_c_message_free_unpacked(msg, NULL);
	return 0;
}

/* The request is complete (and the call dispatched): carry on according to
 * the kind of method. */
static void
server_handle_remote_end(Call *c)
{
	if (c->done || !c->dispatched)
		return;
	if (c->fhdr_got != 0 || c->in_msg) {
		server_finish(c, PROTOBUF_C_GRPC2_INTERNAL, "truncated message");
		return;
	}
	if (c->stream_flags & PROTOBUF_C_INPUT_STREAMING) {
		/* The stream is detached before it is ended, so the handler
		 * may respond (and so finish the call) from inside send(). */
		server_end_sink(c);
	} else if (!c->got_request) {
		server_finish(c, PROTOBUF_C_GRPC2_UNIMPLEMENTED, "missing request");
	} else {
		ProtobufCMessage *request = c->request;
		c->request = NULL;
		server_invoke(c, request);
		protobuf_c_message_free_unpacked(request, NULL);
	}
}

static void
server_on_remote_end(Call *c)
{
	if (c->remote_ended)
		return;
	c->remote_ended = 1;
	server_handle_remote_end(c);
}

/* The deadline is the client's grpc-timeout (capped by max_timeout_ms), or
 * the default if it sent none. */
static void
server_arm_deadline(Call *c)
{
	const ProtobufCGrpc2ConnOptions *o = &c->conn->options;
	uint64_t timeout_ns;

	if (c->has_timeout) {
		timeout_ns = c->timeout_ns;
		if (o->max_timeout_ms > 0 &&
		    timeout_ns > o->max_timeout_ms * 1000000u)
			timeout_ns = o->max_timeout_ms * 1000000u;
	} else if (o->default_timeout_ms > 0) {
		timeout_ns = o->default_timeout_ms * 1000000u;
	} else {
		return;
	}
	c->deadline_ns = add_sat(now_ns(), timeout_ns);
	if (c->deadline_ns == 0)
		c->deadline_ns = 1;
}

/* ---- authentication of server calls ----
 *
 * After the method is found, an authenticator (if the server has one) is
 * asked. If it answers before check() returns, which is the cheap and common
 * case, the call carries on from there with nothing allocated or deferred. If
 * not, the call waits: the data that arrives meanwhile is held (and the flow
 * control credit for it kept back, so that it is bounded by one window) until
 * the verdict is in, then replayed. */

#define HELD_MAX ((size_t) 1024 * 1024)

/* Take what was held, giving the flow-control credit back. The caller owns
 * the returned bytes. */
static uint8_t *
server_take_held(Call *c, size_t *len)
{
	uint8_t *data = c->held;

	*len = c->held_len;
	c->held = NULL;
	c->held_len = c->held_cap = 0;
	if (*len > 0 && c->conn != NULL && c->conn->ng != NULL) {
		if (!c->stream_gone)
			nghttp2_conn_extend_max_stream_offset(c->conn->ng,
							      c->stream_id, *len);
		nghttp2_conn_extend_max_offset(c->conn->ng, *len);
	}
	return data;
}

/* The call may go ahead: reply, invoke the handler, replay what was held. */
static void
server_start_dispatch(Call *c, int fin)
{
	c->dispatched = 1;
	server_reply_headers(c);
	if (c->stream_flags & PROTOBUF_C_INPUT_STREAMING) {
		server_invoke(c, NULL);
		if (c->sink == NULL && !c->done)
			server_finish(c, PROTOBUF_C_GRPC2_INTERNAL, "no input stream");
	}
	if (c->held_len > 0) {
		size_t len;
		uint8_t *data = server_take_held(c, &len);

		if (!c->done) {
			int rv = call_feed(c, data, len, server_on_message);

			if (rv != 0)
				server_finish(c, rv, "bad request message");
		}
		free(data);
	}
	if (c->remote_ended)
		server_handle_remote_end(c);
	else if (fin)
		server_on_remote_end(c);
}

/* The verdict is in c->auth_status, c->auth_message and c->principal. */
static void
server_auth_verdict(Call *c)
{
	if (c->done || c->conn == NULL) {	/* nobody is waiting any more */
		size_t len;

		free(server_take_held(c, &len));
		return;
	}
	if (c->auth_status != PROTOBUF_C_GRPC2_OK) {
		size_t len;

		free(server_take_held(c, &len));
		server_finish(c, c->auth_status,
			      c->auth_message != NULL ? c->auth_message
						      : "not authorized");
		return;
	}
	server_start_dispatch(c, c->headers_fin);
}

/* What the authenticator calls. */
static void
server_auth_done(void *data, int status, const char *message,
		 const char *principal)
{
	Call *c = data;
	int fast = c->auth_state == AUTH_CHECKING;

	if (!fast && c->auth_state != AUTH_PENDING)
		return;				/* answered twice: ignore */
	c->auth_status = status;
	free(c->auth_message);
	c->auth_message = message != NULL ? xstrdup(message) : NULL;
	free(c->principal);
	c->principal = (status == PROTOBUF_C_GRPC2_OK && principal != NULL)
		? xstrdup(principal) : NULL;
	if (fast) {
		c->auth_state = AUTH_REPLIED;	/* check() has not returned yet */
		return;
	}
	c->auth_state = AUTH_DONE;
	server_auth_verdict(c);
	call_unref(c);				/* the reference taken when it went pending */
}

static void
server_auth_begin(Call *c, ProtobufCGrpc2ServerAuth *auth, int fin)
{
	ProtobufCGrpc2Header small[8], *headers = small;
	ProtobufCGrpc2CallInfo info;
	size_t i, n = c->n_auth_headers;

	if (n > sizeof small / sizeof small[0]) {
		headers = malloc(n * sizeof(*headers));
		if (headers == NULL) {
			server_finish(c, PROTOBUF_C_GRPC2_RESOURCE_EXHAUSTED,
				      "out of memory");
			return;
		}
	}
	for (i = 0; i < n; i++) {
		headers[i].name = c->auth_names[i];
		headers[i].value = c->auth_values[i];
	}
	memset(&info, 0, sizeof info);
	info.struct_size = sizeof info;
	info.service = c->service->descriptor;
	info.method = c->method;
	info.peer = c->conn->peer;
	c->headers_fin = fin;
	c->auth_state = AUTH_CHECKING;
	auth->check(auth, &info, headers, n, server_auth_done, c);
	if (headers != small)
		free(headers);
	if (c->auth_state == AUTH_REPLIED) {	/* the fast path */
		c->auth_state = AUTH_DONE;
		server_auth_verdict(c);
		return;
	}
	c->auth_state = AUTH_PENDING;
	c->refs++;				/* held until the answer comes */
}

/* Request headers are complete: validate and look up the method. */
static void
server_on_headers_end(Call *c, int fin)
{
	ProtobufCGrpc2Server *srv = c->conn->server;
	const char *p = c->path;
	const char *slash;
	size_t i, j;

	server_arm_deadline(c);
	if (!c->saw_content_type && c->http_error == 0)
		c->http_error = 415;
	if (c->http_error != 0) {
		server_http_error(c, c->http_error);
		return;
	}
	if (p == NULL || p[0] != '/') {
		server_finish(c, PROTOBUF_C_GRPC2_UNIMPLEMENTED, "bad path");
		return;
	}
	p++;
	slash = strrchr(p, '/');
	if (slash == NULL) {
		server_finish(c, PROTOBUF_C_GRPC2_UNIMPLEMENTED, "bad path");
		return;
	}
	for (i = 0; srv != NULL && i < srv->n_services; i++) {
		const ProtobufCServiceDescriptor *d = srv->services[i]->descriptor;
		if (strlen(d->name) != (size_t) (slash - p) ||
		    strncmp(d->name, p, (size_t) (slash - p)) != 0)
			continue;
		for (j = 0; j < d->n_methods; j++) {
			if (d->methods[j].name != NULL &&
			    strcmp(d->methods[j].name, slash + 1) == 0) {
				c->service = srv->services[i];
				c->method = &d->methods[j];
				c->method_index = (unsigned) j;
				c->stream_flags = d->methods[j].flags &
						  PROTOBUF_C_STREAMING_MASK;
				goto found;
			}
		}
		server_finish(c, PROTOBUF_C_GRPC2_UNIMPLEMENTED, "unknown method");
		return;
	}
	server_finish(c, PROTOBUF_C_GRPC2_UNIMPLEMENTED, "unknown service");
	return;

found:
	if (srv->auth != NULL) {
		server_auth_begin(c, srv->auth, fin);
		return;
	}
	server_start_dispatch(c, fin);
}

/* ------------------------------------------------------------------ */
/* client side */

/* gRPC status for an HTTP response status that carried no grpc-status
 * (see the "HTTP to gRPC Status Code Mapping" document). */
static int
status_from_http(int http_status)
{
	switch (http_status) {
	case 400: return PROTOBUF_C_GRPC2_INTERNAL;
	case 401: return PROTOBUF_C_GRPC2_UNAUTHENTICATED;
	case 403: return PROTOBUF_C_GRPC2_PERMISSION_DENIED;
	case 404: return PROTOBUF_C_GRPC2_UNIMPLEMENTED;
	case 429: case 502: case 503: case 504:
		return PROTOBUF_C_GRPC2_UNAVAILABLE;
	default: return PROTOBUF_C_GRPC2_UNKNOWN;
	}
}

/* gRPC status for an HTTP/2 RST_STREAM / error code from the peer. */
static int
status_from_http2_error(uint32_t error_code)
{
	switch (error_code) {
	case 7:  return PROTOBUF_C_GRPC2_UNAVAILABLE;		/* REFUSED_STREAM */
	case 8:  return PROTOBUF_C_GRPC2_CANCELLED;		/* CANCEL */
	case 11: return PROTOBUF_C_GRPC2_RESOURCE_EXHAUSTED;	/* ENHANCE_YOUR_CALM */
	case 12: return PROTOBUF_C_GRPC2_PERMISSION_DENIED;	/* INADEQUATE_SECURITY */
	default: return PROTOBUF_C_GRPC2_INTERNAL;
	}
}

static void
client_complete(Call *c, int status, const char *message)
{
	if (c->done)
		return;
	c->done = 1;
	call_report_done(c, status, message);
	if (c->closure != NULL) {
		if ((c->stream_flags & PROTOBUF_C_OUTPUT_STREAMING) ||
		    !c->delivered)
			c->closure(NULL, c->closure_data);
	}
}

static int
client_on_message(Call *c, const uint8_t *data, size_t len)
{
	ProtobufCMessage *msg;

	if (c->done)
		return 0;
	if (!(c->stream_flags & PROTOBUF_C_OUTPUT_STREAMING) && c->delivered)
		return PROTOBUF_C_GRPC2_INTERNAL;
	msg = protobuf_c_message_unpack(c->method->output, NULL, len, data);
	if (msg == NULL)
		return PROTOBUF_C_GRPC2_INTERNAL;
	c->delivered = 1;
	if (c->closure != NULL)
		c->closure(msg, c->closure_data);
	protobuf_c_message_free_unpacked(msg, NULL);
	return 0;
}

static void
client_finish_from_trailers(Call *c)
{
	if (c->done)
		return;
	if (c->grpc_status < 0) {
		if (c->http_status != 200 && c->http_status != 0) {
			char text[32];
			snprintf(text, sizeof text, "HTTP %d", c->http_status);
			client_complete(c, status_from_http(c->http_status), text);
		} else {
			client_complete(c, PROTOBUF_C_GRPC2_INTERNAL, "no grpc-status");
		}
		return;
	}
	client_complete(c, c->grpc_status, c->grpc_message);
	/* The server is done; if we are still sending, stop (the server may
	 * have answered without reading all the requests). */
	if (!c->out_finished && c->conn != NULL && !c->stream_gone) {
		c->out_finished = 1;
		nghttp2_conn_shutdown_stream(c->conn->ng, 0, c->stream_id, 0);
	}
}

/* The ProtobufCInputStream of a client call. send(NULL) ends the request
 * stream (half-closes the call) and releases the stream object. */
static protobuf_c_boolean
client_sink_send(ProtobufCInputStream *s, const ProtobufCMessage *msg)
{
	Call *c = ((ClientSink *) s)->call;
	int ok = 0;

	if (msg != NULL) {
		if (c->conn == NULL || c->stream_gone || c->out_finished || c->done)
			return 0;
		return call_queue_message(c, msg) == 0;
	}
	if (c->conn != NULL && !c->stream_gone && !c->out_finished && !c->done) {
		call_finish_output(c);
		ok = 1;
	}
	free(s);
	call_unref(c);
	return ok;
}

/* ---- sending a client call's request ----
 *
 * The request is submitted to nghttp2 either at once, or, when the client has
 * call credentials, from their `done` function (which may be called before
 * `get` returns: then it all happens inside the invoke, as without
 * credentials). The headers of the call come from the Call, so that they
 * survive the wait. */

/* A header the credentials may supply: lower case, no pseudo-headers, nothing
 * that the library sets itself, and a value that fits on one line. */
static int
credential_header_ok(const ProtobufCGrpc2Header *h)
{
	const char *p;

	if (h->name == NULL || h->value == NULL || h->name[0] == 0 ||
	    h->name[0] == ':' || strncmp(h->name, "grpc-", 5) == 0 ||
	    strcmp(h->name, "content-type") == 0 || strcmp(h->name, "te") == 0 ||
	    strcmp(h->name, "user-agent") == 0)
		return 0;
	for (p = h->name; *p != 0; p++)
		if (*p <= ' ' || *p >= 127 || (*p >= 'A' && *p <= 'Z') || *p == ':')
			return 0;
	for (p = h->value; *p != 0; p++)
		if ((unsigned char) *p < ' ' && *p != '\t')
			return 0;
	return 1;
}

/* Submit the request with the extra headers; returns the stream id or a
 * negative nghttp2 error. */
static int64_t
client_submit(Call *c, const ProtobufCGrpc2Header *extra, size_t n_extra)
{
	ProtobufCGrpc2Conn *conn = c->conn;
	nghttp2_nv small[16], *nva = small;
	size_t nnv = 0, i;
	char timeout_hdr[16];
	nghttp2_data_reader dr;
	int64_t sid;

	if (n_extra + 8 > sizeof small / sizeof small[0]) {
		nva = malloc((n_extra + 8) * sizeof(*nva));
		if (nva == NULL)
			return NGHTTP2_ERR_NOMEM;
	}
	nva[nnv++] = nv_lit(":method", "POST");
	nva[nnv++] = nv_lit(":scheme", conn->scheme);
	nva[nnv++] = nv_lit(":path", c->req_path);
	nva[nnv++] = nv_lit(":authority", conn->authority);
	if (c->req_timeout_ms > 0) {
		/* the spec wants grpc-timeout right after the pseudo-headers */
		format_grpc_timeout(c->req_timeout_ms, timeout_hdr);
		nva[nnv++] = nv_lit("grpc-timeout", timeout_hdr);
	}
	nva[nnv++] = nv_lit("content-type", "application/grpc");
	nva[nnv++] = nv_lit("te", "trailers");
	nva[nnv++] = nv_lit("user-agent", "protobuf-c-grpc2");
	for (i = 0; i < n_extra; i++) {
		nghttp2_nv *nv = &nva[nnv++];

		nv->name = (const uint8_t *) extra[i].name;
		nv->namelen = strlen(extra[i].name);
		nv->value = (const uint8_t *) extra[i].value;
		nv->valuelen = strlen(extra[i].value);
		nv->flags = NGHTTP2_NV_FLAG_NEVER_INDEX;	/* both are copied */
	}
	dr.read_data = read_data_cb;
	sid = nghttp2_conn_submit_request(conn->ng, nva, nnv, &dr, c);
	if (nva != small)
		free(nva);
	return sid;
}

/* The call cannot be sent. If the stream's reference is still held, `drop` it:
 * the invoke does that itself when this is called from inside it. */
static void
client_fail_unsent(Call *c, int status, const char *message, int drop)
{
	int had_stream_ref = !c->stream_gone;

	c->stream_gone = 1;		/* there never was a stream */
	client_complete(c, status, message);
	if (drop && had_stream_ref)
		call_unref(c);
}

/* What the credentials call. */
static void
client_credentials_done(void *data, int status, const char *message,
			const ProtobufCGrpc2Header *headers, size_t n_headers)
{
	Call *c = data;
	int fast = c->cred_state == CRED_CHECKING;
	size_t i;
	int64_t sid;

	if (!fast && c->cred_state != CRED_PENDING)
		return;				/* answered twice: ignore */
	c->cred_state = CRED_DONE;
	if (c->done || c->conn == NULL)
		goto out;		/* nobody is waiting any more */
	if (status == PROTOBUF_C_GRPC2_OK) {
		for (i = 0; i < n_headers; i++)
			if (!credential_header_ok(&headers[i])) {
				status = PROTOBUF_C_GRPC2_INTERNAL;
				message = "invalid credentials header";
				break;
			}
	}
	if (status != PROTOBUF_C_GRPC2_OK) {
		client_fail_unsent(c, status,
				   message != NULL ? message : "credentials failed",
				   !fast);
		goto out;
	}
	sid = client_submit(c, headers, n_headers);
	if (sid < 0) {
		client_fail_unsent(c, PROTOBUF_C_GRPC2_UNAVAILABLE,
				   nghttp2_strerror((int) sid), !fast);
		goto out;
	}
	c->stream_id = sid;
out:
	if (!fast)
		call_unref(c);			/* the reference it was waiting with */
}

ProtobufCInputStream *
protobuf_c_grpc2__client_invoke_local(ProtobufCService *service,
				      uint64_t timeout_ms,
				      unsigned index_and_flags,
				      const ProtobufCMessage *input,
				      ProtobufCClosure closure,
				      void *closure_data)
{
	ClientService *cs = (ClientService *) service;
	ProtobufCGrpc2Conn *conn = cs->client
		? protobuf_c_grpc2__client_acquire_conn(cs->client) : cs->conn;
	unsigned index = index_and_flags & ~PROTOBUF_C_STREAMING_MASK;
	unsigned flags = index_and_flags & PROTOBUF_C_STREAMING_MASK;
	const ProtobufCServiceDescriptor *d = service->descriptor;
	const ProtobufCMethodDescriptor *m;
	Call *c;
	ClientSink *sink = NULL;
	ProtobufCGrpc2CallCredentials *creds;
	char *path;
	size_t plen;
	int64_t sid;

	assert(index < d->n_methods);
	m = &d->methods[index];
	assert(flags == (m->flags & PROTOBUF_C_STREAMING_MASK));

	if (conn == NULL && cs->client != NULL && m->name != NULL) {
		/* the client is backing off after a failed connection attempt */
		const ProtobufCGrpc2ConnOptions *o =
			protobuf_c_grpc2__client_options(cs->client);
		if (o->on_call_done != NULL)
			o->on_call_done(m, PROTOBUF_C_GRPC2_UNAVAILABLE,
					"connection failed; backing off",
					o->user_data);
		if (closure != NULL)
			closure(NULL, closure_data);
		return NULL;
	}
	if (conn == NULL || conn->ng == NULL || m->name == NULL || d->name == NULL) {
		if (closure != NULL)
			closure(NULL, closure_data);
		return NULL;
	}
	plen = strlen(d->name) + strlen(m->name) + 3;
	path = malloc(plen);
	c = call_new(conn, 0);
	if (path == NULL || c == NULL) {
		free(path);
		if (c != NULL)
			call_unref(c);
		if (closure != NULL)
			closure(NULL, closure_data);
		return NULL;
	}
	snprintf(path, plen, "/%s/%s", d->name, m->name);
	c->method = m;
	c->method_index = index;
	c->stream_flags = flags;
	c->closure = closure;
	c->closure_data = closure_data;

	if (input != NULL && call_queue_message(c, input) != 0) {
		/* can't happen before submission except for size/ENOMEM */
		c->req_path = path;
		c->stream_gone = 1;
		client_complete(c, PROTOBUF_C_GRPC2_INTERNAL, "cannot send request");
		call_unref(c);
		return NULL;
	}
	if (!(flags & PROTOBUF_C_INPUT_STREAMING))
		c->out_finished = 1;

	c->req_path = path;		/* owned by the call from here on */
	c->req_timeout_ms = timeout_ms;
	if (timeout_ms > 0)
		c->deadline_ns = add_sat(now_ns(), timeout_ms * 1000000u);

	creds = cs->client != NULL ? protobuf_c_grpc2__client_credentials(cs->client)
				   : NULL;
	if (creds == NULL) {
		sid = client_submit(c, NULL, 0);
		if (sid < 0) {
			c->stream_gone = 1;
			client_complete(c, PROTOBUF_C_GRPC2_UNAVAILABLE,
					nghttp2_strerror((int) sid));
			call_unref(c);
			return NULL;
		}
		c->stream_id = sid;
	} else {
		ProtobufCGrpc2CallInfo info;

		memset(&info, 0, sizeof info);
		info.struct_size = sizeof info;
		info.service = d;
		info.method = m;
		c->cred_state = CRED_CHECKING;
		creds->get(creds, protobuf_c_grpc2__client_io(cs->client), &info,
			   client_credentials_done, c);
		if (c->cred_state == CRED_CHECKING) {
			/* not answered yet: the call waits, with a reference
			 * for the answer to drop */
			c->cred_state = CRED_PENDING;
			c->refs++;
		} else if (c->done) {
			/* answered at once, with a failure: reported by now */
			call_unref(c);
			return NULL;
		}
	}

	if (flags & PROTOBUF_C_INPUT_STREAMING) {
		sink = calloc(1, sizeof(*sink));
		if (sink == NULL) {
			if (c->stream_id >= 0)
				nghttp2_conn_shutdown_stream(conn->ng, 0,
							     c->stream_id,
							     HTTP2_CANCEL);
			else
				c->out_finished = 1;	/* sends nothing */
			return NULL;
		}
		sink->base.send = client_sink_send;
		sink->base.input_descriptor = m->input;
		sink->call = c;
		c->refs++;
		return &sink->base;
	}
	return NULL;
}

static ProtobufCInputStream *
client_invoke(ProtobufCService *service, unsigned index_and_flags,
	      const ProtobufCMessage *input, ProtobufCClosure closure,
	      void *closure_data)
{
	ClientService *cs = (ClientService *) service;

	if (cs->client != NULL && protobuf_c_grpc2__client_is_foreign(cs->client))
		return protobuf_c_grpc2__client_invoke_foreign(
			cs->client, service, index_and_flags, cs->timeout_ms,
			input, closure, closure_data);
	return protobuf_c_grpc2__client_invoke_local(
		service, cs->timeout_ms, index_and_flags, input, closure,
		closure_data);
}

static void
client_service_destroy(ProtobufCService *service)
{
	(void) service;		/* owned by the connection */
}

ProtobufCService *
protobuf_c_grpc2_conn_client_service(ProtobufCGrpc2Conn *conn,
			  const ProtobufCServiceDescriptor *descriptor)
{
	ClientService *cs;

	if (conn == NULL || conn->is_server || descriptor == NULL)
		return NULL;
	cs = calloc(1, sizeof(*cs));
	if (cs == NULL)
		return NULL;
	cs->base.descriptor = descriptor;
	cs->timeout_ms = conn->options.default_timeout_ms;
	cs->base.invoke = client_invoke;
	cs->base.destroy = client_service_destroy;
	cs->conn = conn;
	cs->next = conn->client_services;
	conn->client_services = cs;
	return &cs->base;
}

ProtobufCService *
protobuf_c_grpc2__client_service_new(ProtobufCGrpc2Client *client,
				     const ProtobufCServiceDescriptor *descriptor,
				     uint64_t timeout_ms)
{
	ClientService *cs = calloc(1, sizeof(*cs));

	if (cs == NULL)
		return NULL;
	cs->base.descriptor = descriptor;
	cs->base.invoke = client_invoke;
	cs->base.destroy = client_service_destroy;
	cs->timeout_ms = timeout_ms;
	cs->client = client;
	return &cs->base;
}

void
protobuf_c_grpc2__client_service_free(ProtobufCService *service)
{
	free(service);
}

int
protobuf_c_grpc2_client_service_set_timeout_ms(ProtobufCService *service,
					       uint64_t timeout_ms)
{
	if (service == NULL || service->invoke != client_invoke)
		return -1;
	((ClientService *) service)->timeout_ms = timeout_ms;
	return 0;
}

/* ------------------------------------------------------------------ */
/* nghttp2 callbacks */

static int
begin_headers_cb(nghttp2_conn *ng, int64_t stream_id, void *conn_ud,
		 void *stream_ud)
{
	ProtobufCGrpc2Conn *conn = conn_ud;

	if (conn->is_server && stream_ud == NULL) {
		Call *c = call_new(conn, 1);
		if (c == NULL)
			return NGHTTP2_ERR_CALLBACK_FAILURE;
		c->stream_id = stream_id;
		nghttp2_conn_set_stream_user_data(ng, stream_id, c);
	}
	return 0;
}

static int
recv_header_cb(nghttp2_conn *ng, int64_t stream_id, int32_t token,
	       nghttp2_rcbuf *name, nghttp2_rcbuf *value, uint8_t flags,
	       void *conn_ud, void *stream_ud)
{
	Call *c = stream_ud;
	nghttp2_vec n = nghttp2_rcbuf_get_buf(name);
	nghttp2_vec v = nghttp2_rcbuf_get_buf(value);
	(void) ng; (void) stream_id; (void) flags; (void) conn_ud;

	if (c == NULL)
		return 0;
	if (c->is_server) {
		if (token == NGHTTP2_HPACK_TOKEN__PATH) {
			free(c->path);
			c->path = xstrndup(v.base, v.len);
		} else if (token == NGHTTP2_HPACK_TOKEN__METHOD) {
			if (!(v.len == 4 && memcmp(v.base, "POST", 4) == 0) &&
			    c->http_error == 0)
				c->http_error = 405;
		} else if (n.len == 12 && memcmp(n.base, "grpc-timeout", 12) == 0) {
			/* a malformed value is ignored (no deadline) */
			c->has_timeout =
				parse_grpc_timeout(v.base, v.len, &c->timeout_ns) == 0;
		} else if (n.len == 12 && memcmp(n.base, "content-type", 12) == 0) {
			c->saw_content_type = 1;
			if (!(v.len >= 16 &&
			      memcmp(v.base, "application/grpc", 16) == 0))
				c->http_error = 415;	/* wins over 405 */
		} else if (c->conn != NULL && c->conn->server != NULL &&
			   c->conn->server->n_auth_names > 0) {
			/* a header the authenticator wants (names are lower
			 * case on both sides) */
			const ProtobufCGrpc2Server *srv = c->conn->server;
			size_t i;

			for (i = 0; i < srv->n_auth_names; i++) {
				char **nn, **vv;

				if (strlen(srv->auth_names[i]) != n.len ||
				    memcmp(srv->auth_names[i], n.base, n.len) != 0)
					continue;
				nn = realloc(c->auth_names,
					     (c->n_auth_headers + 1) * sizeof(char *));
				if (nn == NULL)
					break;
				c->auth_names = nn;
				vv = realloc(c->auth_values,
					     (c->n_auth_headers + 1) * sizeof(char *));
				if (vv == NULL)
					break;
				c->auth_values = vv;
				c->auth_names[c->n_auth_headers] =
					xstrndup(n.base, n.len);
				c->auth_values[c->n_auth_headers] =
					xstrndup(v.base, v.len);
				if (c->auth_names[c->n_auth_headers] != NULL &&
				    c->auth_values[c->n_auth_headers] != NULL)
					c->n_auth_headers++;
				else {
					free(c->auth_names[c->n_auth_headers]);
					free(c->auth_values[c->n_auth_headers]);
				}
				break;
			}
		}
		return 0;
	}
	if (token == NGHTTP2_HPACK_TOKEN__STATUS) {
		c->http_status = atoi((const char *) v.base);
	} else if (n.len == 11 && memcmp(n.base, "grpc-status", 11) == 0) {
		char *s = xstrndup(v.base, v.len);
		if (s != NULL) {
			c->grpc_status = atoi(s);
			free(s);
		}
	} else if (n.len == 12 && memcmp(n.base, "grpc-message", 12) == 0) {
		free(c->grpc_message);
		c->grpc_message = pct_decode(v.base, v.len);
	}
	return 0;
}

static int
end_headers_cb(nghttp2_conn *ng, int64_t stream_id, int fin, void *conn_ud,
	       void *stream_ud)
{
	Call *c = stream_ud;
	(void) ng; (void) stream_id; (void) conn_ud;

	if (c == NULL)
		return 0;
	if (c->is_server) {
		if (!c->headers_done && c->conn != NULL) {
			c->headers_done = 1;
			server_on_headers_end(c, fin);
		}
		return 0;
	}
	if (fin) {
		c->remote_ended = 1;
		client_finish_from_trailers(c);
	}
	return 0;
}

static int
end_trailers_cb(nghttp2_conn *ng, int64_t stream_id, int fin, void *conn_ud,
		void *stream_ud)
{
	Call *c = stream_ud;
	(void) ng; (void) stream_id; (void) fin; (void) conn_ud;

	if (c != NULL && !c->is_server)
		client_finish_from_trailers(c);
	return 0;
}

static int
recv_data_cb(nghttp2_conn *ng, int64_t stream_id, const uint8_t *data,
	     size_t datalen, void *conn_ud, void *stream_ud)
{
	Call *c = stream_ud;
	int rv;
	(void) conn_ud;

	if (c != NULL && c->is_server && c->auth_state == AUTH_PENDING &&
	    !c->done && c->conn != NULL) {
		/* waiting for the authenticator: keep the data, and the
		 * flow-control credit for it, until the verdict is in */
		if (c->held_len + datalen > HELD_MAX) {
			server_finish(c, PROTOBUF_C_GRPC2_RESOURCE_EXHAUSTED,
				      "too much data before authentication");
			nghttp2_conn_extend_max_stream_offset(ng, stream_id, datalen);
			nghttp2_conn_extend_max_offset(ng, datalen);
			return 0;
		}
		if (c->held_len + datalen > c->held_cap) {
			size_t cap = c->held_cap ? c->held_cap : 1024;
			uint8_t *nb;

			while (cap < c->held_len + datalen)
				cap *= 2;
			nb = realloc(c->held, cap);
			if (nb == NULL) {
				server_finish(c, PROTOBUF_C_GRPC2_RESOURCE_EXHAUSTED,
					      "out of memory");
				nghttp2_conn_extend_max_stream_offset(ng, stream_id, datalen);
				nghttp2_conn_extend_max_offset(ng, datalen);
				return 0;
			}
			c->held = nb;
			c->held_cap = cap;
		}
		memcpy(c->held + c->held_len, data, datalen);
		c->held_len += datalen;
		return 0;
	}
	nghttp2_conn_extend_max_stream_offset(ng, stream_id, datalen);
	nghttp2_conn_extend_max_offset(ng, datalen);
	if (c == NULL || c->done || c->conn == NULL)
		return 0;
	if (c->is_server) {
		if (!c->dispatched)
			return 0;
		rv = call_feed(c, data, datalen, server_on_message);
		if (rv != 0)
			server_finish(c, rv, "bad request message");
	} else if (c->http_status != 200) {
		return 0;		/* an HTTP error page, not gRPC messages */
	} else {
		rv = call_feed(c, data, datalen, client_on_message);
		if (rv != 0) {
			client_complete(c, rv, "bad response message");
			nghttp2_conn_shutdown_stream(ng, 0, stream_id, HTTP2_CANCEL);
		}
	}
	return 0;
}

static int
end_stream_remote_cb(nghttp2_conn *ng, int64_t stream_id, void *conn_ud,
		     void *stream_ud)
{
	Call *c = stream_ud;
	(void) ng; (void) stream_id; (void) conn_ud;

	if (c == NULL)
		return 0;
	if (c->is_server)
		server_on_remote_end(c);
	else
		c->remote_ended = 1;
	return 0;
}

static int
stream_close_cb(nghttp2_conn *ng, uint32_t flags, int64_t stream_id,
		uint32_t error_code, void *conn_ud, void *stream_ud)
{
	Call *c = stream_ud;
	(void) ng; (void) stream_id; (void) conn_ud;

	if (c == NULL)
		return 0;
	c->stream_gone = 1;
	if (c->is_server) {
		if (!c->done) {
			c->done = 1;
			call_report_done(c, PROTOBUF_C_GRPC2_CANCELLED, "stream closed");
		}
		server_end_sink(c);
		/* No more responses can be sent; drop the closure reference
		 * only when the handler has completed (see server_closure). */
	} else if (!c->done) {
		if (c->grpc_status >= 0)
			client_complete(c, c->grpc_status, c->grpc_message);
		else if (c->http_status != 200 && c->http_status != 0) {
			char text[32];
			snprintf(text, sizeof text, "HTTP %d", c->http_status);
			client_complete(c, status_from_http(c->http_status), text);
		} else if (flags & NGHTTP2_STREAM_CLOSE_FLAG_ERROR_CODE_SET) {
			/* a local cancel, or the peer's RST_STREAM */
			client_complete(c, status_from_http2_error(error_code),
					nghttp2_http2_strerror(error_code));
		} else {
			client_complete(c, PROTOBUF_C_GRPC2_INTERNAL,
					"stream closed without a status");
		}
	}
	call_unref(c);
	return 0;
}

static void
fill_callbacks(nghttp2_callbacks *cb)
{
	memset(cb, 0, sizeof(*cb));
	cb->rand = rand_cb;
	cb->write_stream_data_offset = write_stream_data_offset_cb;
	cb->begin_headers = begin_headers_cb;
	cb->recv_header = recv_header_cb;
	cb->end_headers = end_headers_cb;
	cb->recv_trailer = recv_header_cb;
	cb->end_trailers = end_trailers_cb;
	cb->recv_data = recv_data_cb;
	cb->end_stream_remote = end_stream_remote_cb;
	cb->stream_close = stream_close_cb;
}

/* ------------------------------------------------------------------ */
/* public connection API */

static ProtobufCGrpc2Conn *
conn_new(ProtobufCGrpc2Server *server, const ProtobufCGrpc2ConnOptions *options, int is_server)
{
	ProtobufCGrpc2Conn *conn = calloc(1, sizeof(*conn));
	nghttp2_callbacks cb;
	nghttp2_settings settings;
	int rv;

	if (conn == NULL)
		return NULL;
	if (options != NULL) {
		if (PROTOBUF_C_GRPC2__COPY_OPTIONS(&conn->options, options) != 0) {
			free(conn);
			return NULL;
		}
	} else {
		conn->options.struct_size = sizeof conn->options;
	}
	conn->is_server = is_server;
	conn->server = server;
	conn->max_message_size = conn->options.max_message_size
		? conn->options.max_message_size : DEFAULT_MAX_MESSAGE_SIZE;
	conn->authority = xstrdup(conn->options.authority
				  ? conn->options.authority : "localhost");
	conn->scheme = xstrdup(conn->options.scheme
			       ? conn->options.scheme : "http");
	if (conn->authority == NULL || conn->scheme == NULL) {
		free(conn->authority);
		free(conn->scheme);
		free(conn);
		return NULL;
	}

	fill_callbacks(&cb);
	nghttp2_settings_default(&settings);
	settings.initial_ts = now_ns();
	/* The server lets clients open streams; clients refuse pushes. */
	settings.max_concurrent_streams_remote = is_server ? 100 : 0;
	if (is_server)
		rv = nghttp2_conn_server_new(&conn->ng, &cb, &settings, NULL, conn);
	else
		rv = nghttp2_conn_client_new(&conn->ng, &cb, &settings, NULL, conn);
	if (rv != 0) {
		free(conn->authority);
		free(conn->scheme);
		free(conn);
		return NULL;
	}
	return conn;
}

ProtobufCGrpc2Conn *
protobuf_c_grpc2_conn_server_new(ProtobufCGrpc2Server *server, const ProtobufCGrpc2ConnOptions *options)
{
	return conn_new(server, options, 1);
}

ProtobufCGrpc2Conn *
protobuf_c_grpc2_conn_client_new(const ProtobufCGrpc2ConnOptions *options)
{
	return conn_new(NULL, options, 0);
}

void
protobuf_c_grpc2_conn_free(ProtobufCGrpc2Conn *conn)
{
	ClientService *cs, *next;

	if (conn == NULL)
		return;
	/* Complete or cancel every live call, then detach it. */
	while (conn->calls != NULL) {
		Call *c = conn->calls;
		int stream_ref = !c->stream_gone;	/* else already dropped */
		c->stream_gone = 1;
		if (c->is_server) {
			server_end_sink(c);
			if (!c->done) {
				c->done = 1;
				call_report_done(c, PROTOBUF_C_GRPC2_CANCELLED,
						 "connection closed");
			}
		} else {
			client_complete(c, PROTOBUF_C_GRPC2_UNAVAILABLE, "connection closed");
		}
		call_unlink(c);
		/* A server handler may still call its response closure later;
		 * the closure's reference (if any) keeps the Call alive until
		 * that final invocation. */
		if (stream_ref)
			call_unref(c);	/* the stream's reference */
	}
	for (cs = conn->client_services; cs != NULL; cs = next) {
		next = cs->next;
		free(cs);
	}
	nghttp2_conn_del(conn->ng);
	free(conn->authority);
	free(conn->scheme);
	free(conn->peer);
	free(conn);
}

int
protobuf_c_grpc2_conn_read(ProtobufCGrpc2Conn *conn, const uint8_t *data, size_t len)
{
	return nghttp2_conn_read(conn->ng, data, len, now_ns());
}

ssize_t
protobuf_c_grpc2_conn_write(ProtobufCGrpc2Conn *conn, uint8_t *buf, size_t len)
{
	return nghttp2_conn_write(conn->ng, buf, len, now_ns());
}

/* Earliest call deadline, or 0 if no live call has one. */
static uint64_t
earliest_deadline(const ProtobufCGrpc2Conn *conn)
{
	const Call *c;
	uint64_t best = 0;

	for (c = conn->calls; c != NULL; c = c->next)
		if (!c->done && c->deadline_ns != 0 &&
		    (best == 0 || c->deadline_ns < best))
			best = c->deadline_ns;
	return best;
}

int64_t
protobuf_c_grpc2_conn_get_timeout_ns(const ProtobufCGrpc2Conn *conn)
{
	nghttp2_tstamp expiry = nghttp2_conn_get_expiry(conn->ng);
	uint64_t dl = earliest_deadline(conn);
	nghttp2_tstamp now;

	if (dl != 0 && dl < expiry)
		expiry = dl;
	if (expiry == UINT64_MAX)
		return -1;
	now = now_ns();
	if (expiry <= now)
		return 0;
	return (expiry - now > (nghttp2_tstamp) INT64_MAX)
		? INT64_MAX : (int64_t) (expiry - now);
}

uint64_t
protobuf_c_grpc2_conn_get_expiry(const ProtobufCGrpc2Conn *conn)
{
	int64_t t = protobuf_c_grpc2_conn_get_timeout_ns(conn);
	return t < 0 ? UINT64_MAX : add_sat(now_ns(), (uint64_t) t);
}

static Call *
find_expired_call(const ProtobufCGrpc2Conn *conn, uint64_t now)
{
	Call *c;

	for (c = conn->calls; c != NULL; c = c->next)
		if (!c->done && c->deadline_ns != 0 && c->deadline_ns <= now)
			return c;
	return NULL;
}

int
protobuf_c_grpc2_conn_handle_expiry(ProtobufCGrpc2Conn *conn)
{
	uint64_t now = now_ns();
	int rv = nghttp2_conn_handle_expiry(conn->ng, now);
	Call *c;

	/* Fire expired deadlines one at a time: the callbacks they cause may
	 * change the list of calls. */
	while ((c = find_expired_call(conn, now)) != NULL) {
		c->deadline_ns = 0;
		if (c->is_server) {
			server_finish(c, PROTOBUF_C_GRPC2_DEADLINE_EXCEEDED,
				      "deadline exceeded");
		} else {
			/* cancel the stream, then tell the application */
			if (!c->stream_gone && c->stream_id >= 0)
				nghttp2_conn_shutdown_stream(conn->ng, 0,
							     c->stream_id,
							     HTTP2_CANCEL);
			c->out_finished = 1;
			client_complete(c, PROTOBUF_C_GRPC2_DEADLINE_EXCEEDED,
					"deadline exceeded");
			if (c->stream_id < 0 && !c->stream_gone) {
				/* still waiting for its credentials: there is
				 * no stream to close, so let go of it here */
				c->stream_gone = 1;
				call_unref(c);
			}
		}
	}
	return rv;
}

void
protobuf_c_grpc2__conn_set_peer(ProtobufCGrpc2Conn *conn, const char *peer)
{
	free(conn->peer);
	conn->peer = peer != NULL ? xstrdup(peer) : NULL;
}

/* ------------------------------------------------------------------ */
/* server registry */

void
protobuf_c_grpc2_server_free(ProtobufCGrpc2Server *server)
{
	if (server == NULL)
		return;
	if (server->io != NULL) {
		/* I/O servers shut down asynchronously. */
		protobuf_c_grpc2__server_io_free(server);
		return;
	}
	protobuf_c_grpc2__server_release_auth(server);
	free(server->services);
	free(server);
}

int
protobuf_c_grpc2_server_add_service(ProtobufCGrpc2Server *server, ProtobufCService *service)
{
	const char *name = service->descriptor->name;
	ProtobufCService **n;
	size_t i;

	if (name == NULL)
		return -1;
	for (i = 0; i < server->n_services; i++)
		if (strcmp(server->services[i]->descriptor->name, name) == 0)
			return -1;
	n = realloc(server->services, (server->n_services + 1) * sizeof(*n));
	if (n == NULL)
		return -1;
	server->services = n;
	n[server->n_services++] = service;
	return 0;
}
