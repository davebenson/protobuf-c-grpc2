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

#include "protobuf-c-grpc2.h"

#define DEFAULT_MAX_MESSAGE_SIZE ((size_t) 4 * 1024 * 1024)
#define FRAME_HEADER_LEN 5
#define HTTP2_CANCEL 8u

typedef struct Call Call;
typedef struct ClientService ClientService;

struct Grpc2Server {
	ProtobufCService **services;
	size_t n_services;
};

struct Grpc2Conn {
	nghttp2_conn *ng;
	int is_server;
	Grpc2Server *server;
	Grpc2ConnOptions options;
	char *authority;
	char *scheme;
	size_t max_message_size;
	Call *calls;			/* all live calls, doubly linked */
	ClientService *client_services;
};

struct ClientService {
	ProtobufCService base;		/* must be first */
	Grpc2Conn *conn;
	ClientService *next;
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
	Grpc2Conn *conn;		/* NULL once the connection is gone */
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
	int stream_gone;		/* nghttp2 stream closed */
	int remote_ended;		/* peer finished sending */
	int done;			/* final status determined */

	/* server side */
	char *path;
	int http_ok;			/* request looked like gRPC */
	int headers_done;		/* request headers processed */
	int dispatched;
	int got_request;
	int closure_ref;		/* holds a reference for the closure */
	ProtobufCInputStream *sink;	/* input stream from the handler */

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
grpc2_status_name(int status)
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
call_new(Grpc2Conn *conn, int is_server)
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
	nghttp2_conn_resume_stream(c->conn->ng, c->stream_id);
	return 0;
}

static void
call_finish_output(Call *c)
{
	if (c->out_finished)
		return;
	c->out_finished = 1;
	if (c->conn != NULL && !c->stream_gone)
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
				return GRPC2_UNIMPLEMENTED; /* compression */
			c->msg_len = ((size_t) c->fhdr[1] << 24) |
				     ((size_t) c->fhdr[2] << 16) |
				     ((size_t) c->fhdr[3] << 8) | c->fhdr[4];
			if (c->msg_len > c->conn->max_message_size)
				return GRPC2_RESOURCE_EXHAUSTED;
			c->msg_got = 0;
			c->in_msg = 1;
			free(c->msg);
			c->msg = malloc(c->msg_len ? c->msg_len : 1);
			if (c->msg == NULL)
				return GRPC2_RESOURCE_EXHAUSTED;
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

static void
server_finish(Call *c, int status, const char *message)
{
	nghttp2_nv nva[2];
	char code[16];
	char *enc = NULL;
	size_t n = 1;

	if (c->done || c->conn == NULL)
		return;
	c->done = 1;
	if (c->sink != NULL) {
		ProtobufCInputStream *s = c->sink;
		c->sink = NULL;
		s->vtable->destroy(s);
	}
	if (c->stream_gone)
		return;

	snprintf(code, sizeof code, "%d", status);
	nva[0] = nv_lit("grpc-status", code);
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
			server_finish(c, GRPC2_OK, NULL);
		else
			server_finish(c, GRPC2_UNKNOWN, "method failed");
		server_release_closure(c);
		return;
	}
	if (c->done || c->stream_gone || c->conn == NULL)
		return;
	if (call_queue_message(c, msg) != 0) {
		server_finish(c, GRPC2_INTERNAL, "cannot send response");
		server_release_closure(c);
		return;
	}
	if (!(c->stream_flags & PROTOBUF_C_OUTPUT_STREAMING)) {
		server_finish(c, GRPC2_OK, NULL);
		server_release_closure(c);
	}
}

static void
server_invoke(Call *c, const ProtobufCMessage *input)
{
	c->closure_ref = 1;
	c->refs++;
	c->sink = c->service->invoke(c->service,
				     c->method_index | c->stream_flags, input,
				     (ProtobufCClosure) server_closure, c);
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
		return GRPC2_INVALID_ARGUMENT;
	if (c->stream_flags & PROTOBUF_C_INPUT_STREAMING) {
		if (c->sink != NULL && !c->sink->vtable->send_input(c->sink, msg)) {
			protobuf_c_message_free_unpacked(msg, NULL);
			return GRPC2_INTERNAL;
		}
	} else if (c->got_request) {
		protobuf_c_message_free_unpacked(msg, NULL);
		return GRPC2_INVALID_ARGUMENT;
	} else {
		c->got_request = 1;
		server_invoke(c, msg);
	}
	protobuf_c_message_free_unpacked(msg, NULL);
	return 0;
}

static void
server_on_remote_end(Call *c)
{
	if (c->remote_ended)
		return;
	c->remote_ended = 1;
	if (c->done || !c->dispatched)
		return;
	if (c->fhdr_got != 0 || c->in_msg) {
		server_finish(c, GRPC2_INVALID_ARGUMENT, "truncated message");
		return;
	}
	if (c->stream_flags & PROTOBUF_C_INPUT_STREAMING) {
		if (c->sink != NULL) {
			ProtobufCInputStream *s = c->sink;
			/* Keep c->sink set: the handler may respond (and so
			 * finish the call) from inside send_input(). */
			s->vtable->send_input(s, NULL);
			if (c->sink == s) {
				c->sink = NULL;
				s->vtable->destroy(s);
			}
		}
	} else if (!c->got_request) {
		server_finish(c, GRPC2_INVALID_ARGUMENT, "missing request");
	}
}

/* Request headers are complete: validate and look up the method. */
static void
server_on_headers_end(Call *c, int fin)
{
	Grpc2Server *srv = c->conn->server;
	const char *p = c->path;
	const char *slash;
	size_t i, j;

	server_reply_headers(c);
	if (!c->http_ok || p == NULL || p[0] != '/') {
		server_finish(c, GRPC2_INVALID_ARGUMENT, "not a gRPC request");
		return;
	}
	p++;
	slash = strrchr(p, '/');
	if (slash == NULL) {
		server_finish(c, GRPC2_UNIMPLEMENTED, "bad path");
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
		server_finish(c, GRPC2_UNIMPLEMENTED, "unknown method");
		return;
	}
	server_finish(c, GRPC2_UNIMPLEMENTED, "unknown service");
	return;

found:
	c->dispatched = 1;
	if (c->stream_flags & PROTOBUF_C_INPUT_STREAMING) {
		server_invoke(c, NULL);
		if (c->sink == NULL && !c->done)
			server_finish(c, GRPC2_INTERNAL, "no input stream");
	}
	if (fin)
		server_on_remote_end(c);
}

/* ------------------------------------------------------------------ */
/* client side */

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
		return GRPC2_INTERNAL;
	msg = protobuf_c_message_unpack(c->method->output, NULL, len, data);
	if (msg == NULL)
		return GRPC2_INTERNAL;
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
		if (c->http_status != 200)
			client_complete(c, GRPC2_UNAVAILABLE, "bad HTTP status");
		else
			client_complete(c, GRPC2_INTERNAL, "no grpc-status");
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

/* The ProtobufCInputStream of a client call. */
static protobuf_c_boolean
client_sink_send_input(ProtobufCInputStream *s, const ProtobufCMessage *msg)
{
	Call *c = ((ClientSink *) s)->call;

	if (c->conn == NULL || c->stream_gone || c->out_finished || c->done)
		return 0;
	if (msg == NULL) {
		call_finish_output(c);
		return 1;
	}
	return call_queue_message(c, msg) == 0;
}

static void
client_sink_destroy(ProtobufCInputStream *s)
{
	Call *c = ((ClientSink *) s)->call;

	if (!c->out_finished && c->conn != NULL && !c->stream_gone) {
		/* not ended by the caller: cancel the call */
		nghttp2_conn_shutdown_stream(c->conn->ng, 0, c->stream_id,
					     HTTP2_CANCEL);
	}
	free(s);
	call_unref(c);
}

static const ProtobufCInputStreamVTable client_sink_vtable = {
	client_sink_send_input,
	client_sink_destroy
};

static ProtobufCInputStream *
client_invoke(ProtobufCService *service, unsigned index_and_flags,
	      const ProtobufCMessage *input, ProtobufCClosure closure,
	      void *closure_data)
{
	ClientService *cs = (ClientService *) service;
	Grpc2Conn *conn = cs->conn;
	unsigned index = index_and_flags & ~PROTOBUF_C_STREAMING_MASK;
	unsigned flags = index_and_flags & PROTOBUF_C_STREAMING_MASK;
	const ProtobufCServiceDescriptor *d = service->descriptor;
	const ProtobufCMethodDescriptor *m;
	Call *c;
	ClientSink *sink = NULL;
	nghttp2_nv nva[7];
	nghttp2_data_reader dr;
	char *path;
	size_t plen;
	int64_t sid;

	assert(index < d->n_methods);
	m = &d->methods[index];
	assert(flags == (m->flags & PROTOBUF_C_STREAMING_MASK));

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
		free(path);
		c->stream_gone = 1;
		client_complete(c, GRPC2_INTERNAL, "cannot send request");
		call_unref(c);
		return NULL;
	}
	if (!(flags & PROTOBUF_C_INPUT_STREAMING))
		c->out_finished = 1;

	nva[0] = nv_lit(":method", "POST");
	nva[1] = nv_lit(":scheme", conn->scheme);
	nva[2] = nv_lit(":path", path);
	nva[3] = nv_lit(":authority", conn->authority);
	nva[4] = nv_lit("content-type", "application/grpc");
	nva[5] = nv_lit("te", "trailers");
	nva[6] = nv_lit("user-agent", "protobuf-c-grpc2");
	dr.read_data = read_data_cb;
	sid = nghttp2_conn_submit_request(conn->ng, nva, 7, &dr, c);
	free(path);
	if (sid < 0) {
		c->stream_gone = 1;
		client_complete(c, GRPC2_UNAVAILABLE, nghttp2_strerror((int) sid));
		call_unref(c);
		return NULL;
	}
	c->stream_id = sid;

	if (flags & PROTOBUF_C_INPUT_STREAMING) {
		sink = calloc(1, sizeof(*sink));
		if (sink == NULL) {
			nghttp2_conn_shutdown_stream(conn->ng, 0, sid, HTTP2_CANCEL);
			return NULL;
		}
		sink->base.vtable = &client_sink_vtable;
		sink->call = c;
		c->refs++;
		return &sink->base;
	}
	return NULL;
}

static void
client_service_destroy(ProtobufCService *service)
{
	(void) service;		/* owned by the connection */
}

ProtobufCService *
grpc2_conn_client_service(Grpc2Conn *conn,
			  const ProtobufCServiceDescriptor *descriptor)
{
	ClientService *cs;

	if (conn == NULL || conn->is_server || descriptor == NULL)
		return NULL;
	cs = calloc(1, sizeof(*cs));
	if (cs == NULL)
		return NULL;
	cs->base.descriptor = descriptor;
	cs->base.invoke = client_invoke;
	cs->base.destroy = client_service_destroy;
	cs->conn = conn;
	cs->next = conn->client_services;
	conn->client_services = cs;
	return &cs->base;
}

/* ------------------------------------------------------------------ */
/* nghttp2 callbacks */

static int
begin_headers_cb(nghttp2_conn *ng, int64_t stream_id, void *conn_ud,
		 void *stream_ud)
{
	Grpc2Conn *conn = conn_ud;

	if (conn->is_server && stream_ud == NULL) {
		Call *c = call_new(conn, 1);
		if (c == NULL)
			return NGHTTP2_ERR_CALLBACK_FAILURE;
		c->stream_id = stream_id;
		c->http_ok = 1;
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
			if (!(v.len == 4 && memcmp(v.base, "POST", 4) == 0))
				c->http_ok = 0;
		} else if (n.len == 12 && memcmp(n.base, "content-type", 12) == 0) {
			if (!(v.len >= 16 &&
			      memcmp(v.base, "application/grpc", 16) == 0))
				c->http_ok = 0;
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
			call_report_done(c, GRPC2_CANCELLED, "stream closed");
		}
		if (c->sink != NULL) {
			ProtobufCInputStream *s = c->sink;
			c->sink = NULL;
			s->vtable->destroy(s);
		}
		/* No more responses can be sent; drop the closure reference
		 * only when the handler has completed (see server_closure). */
	} else if (!c->done) {
		if (c->grpc_status >= 0)
			client_complete(c, c->grpc_status, c->grpc_message);
		else if ((flags & NGHTTP2_STREAM_CLOSE_FLAG_ERROR_CODE_SET_LOCAL) &&
			 error_code == HTTP2_CANCEL)
			client_complete(c, GRPC2_CANCELLED, "cancelled");
		else
			client_complete(c, GRPC2_UNAVAILABLE, "stream closed");
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

static Grpc2Conn *
conn_new(Grpc2Server *server, const Grpc2ConnOptions *options, int is_server)
{
	Grpc2Conn *conn = calloc(1, sizeof(*conn));
	nghttp2_callbacks cb;
	nghttp2_settings settings;
	int rv;

	if (conn == NULL)
		return NULL;
	if (options != NULL)
		conn->options = *options;
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

Grpc2Conn *
grpc2_conn_server_new(Grpc2Server *server, const Grpc2ConnOptions *options)
{
	return conn_new(server, options, 1);
}

Grpc2Conn *
grpc2_conn_client_new(const Grpc2ConnOptions *options)
{
	return conn_new(NULL, options, 0);
}

void
grpc2_conn_free(Grpc2Conn *conn)
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
			if (c->sink != NULL) {
				ProtobufCInputStream *s = c->sink;
				c->sink = NULL;
				s->vtable->destroy(s);
			}
			c->done = 1;
		} else {
			client_complete(c, GRPC2_UNAVAILABLE, "connection closed");
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
	free(conn);
}

int
grpc2_conn_read(Grpc2Conn *conn, const uint8_t *data, size_t len)
{
	return nghttp2_conn_read(conn->ng, data, len, now_ns());
}

ssize_t
grpc2_conn_write(Grpc2Conn *conn, uint8_t *buf, size_t len)
{
	return nghttp2_conn_write(conn->ng, buf, len, now_ns());
}

uint64_t
grpc2_conn_get_expiry(const Grpc2Conn *conn)
{
	return nghttp2_conn_get_expiry(conn->ng);
}

int
grpc2_conn_handle_expiry(Grpc2Conn *conn)
{
	return nghttp2_conn_handle_expiry(conn->ng, now_ns());
}

/* ------------------------------------------------------------------ */
/* server registry */

Grpc2Server *
grpc2_server_new(void)
{
	return calloc(1, sizeof(Grpc2Server));
}

void
grpc2_server_free(Grpc2Server *server)
{
	if (server == NULL)
		return;
	free(server->services);
	free(server);
}

int
grpc2_server_add_service(Grpc2Server *server, ProtobufCService *service)
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
