/*
 * uv-grpc2.h: glue between a libuv TCP stream and a ProtobufCGrpc2Conn.
 *
 * This is the only code that knows about both libuv and grpc2; it shows how
 * to drive the sans-I/O grpc2 API from an event loop:
 *
 *   - bytes read from the socket go to protobuf_c_grpc2_conn_read();
 *   - before the loop blocks (a uv_prepare handle, which runs after the
 *     timers of the iteration) each connection is "flushed": protobuf_c_grpc2_conn_write() is called until it returns 0 and the
 *     bytes are written to the socket. Doing it here, rather than after each
 *     callback, picks up output produced from timers and other callbacks;
 *   - the library's timer (SETTINGS ack timeout) is mapped to a uv_timer.
 *
 * Header-only (static functions); include it from exactly one file.
 */
#ifndef UV_GRPC2_H
#define UV_GRPC2_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include <protobuf-c-grpc2/protobuf-c-grpc2.h>

typedef struct UvConn UvConn;
struct UvConn {
	uv_tcp_t tcp;			/* must be first */
	uv_timer_t timer;
	ProtobufCGrpc2Conn *conn;
	UvConn *next, **pprev;
	int closing;
	int handles_open;
	char rbuf[16384];
	void (*on_closed)(void *arg);	/* optional, called after freeing */
	void *on_closed_arg;
};

static UvConn *uvconn_list;
static uv_prepare_t uvconn_prepare;

static void uvconn_close(UvConn *c);
static void uvconn_timer_cb(uv_timer_t *t);

static void
uvconn_write_done(uv_write_t *req, int status)
{
	/* The UvConn may be gone; the request only owns its buffer. */
	(void) status;
	free(req);
}

/* Send everything grpc2 has queued, and rearm its timer. */
static void
uvconn_flush(UvConn *c)
{
	uint8_t buf[16384];
	ssize_t n;
	int64_t t;

	if (c->closing)
		return;
	while ((n = protobuf_c_grpc2_conn_write(c->conn, buf, sizeof buf)) > 0) {
		uv_write_t *req = malloc(sizeof(*req) + (size_t) n);
		uv_buf_t b;
		if (req == NULL) {
			uvconn_close(c);
			return;
		}
		memcpy((char *) (req + 1), buf, (size_t) n);
		b = uv_buf_init((char *) (req + 1), (unsigned) n);
		if (uv_write(req, (uv_stream_t *) &c->tcp, &b, 1,
			     uvconn_write_done) != 0) {
			free(req);
			uvconn_close(c);
			return;
		}
	}
	if (n < 0) {		/* e.g. GOAWAY handled: connection finished */
		uvconn_close(c);
		return;
	}
	t = protobuf_c_grpc2_conn_get_timeout_ns(c->conn);
	if (t < 0) {
		uv_timer_stop(&c->timer);
	} else {
		uint64_t ms = (uint64_t) t / 1000000u + 1;
		uv_timer_start(&c->timer, uvconn_timer_cb, ms, 0);
	}
}

static void
uvconn_prepare_cb(uv_prepare_t *h)
{
	UvConn *c;
	(void) h;
	for (c = uvconn_list; c != NULL; c = c->next)
		uvconn_flush(c);
}

static void
uvconn_timer_cb(uv_timer_t *t)
{
	UvConn *c = t->data;
	if (!c->closing && protobuf_c_grpc2_conn_handle_expiry(c->conn) < 0)
		uvconn_close(c);
}

static void
uvconn_alloc_cb(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
	UvConn *c = (UvConn *) h;
	(void) suggested;
	*buf = uv_buf_init(c->rbuf, sizeof c->rbuf);
}

static void
uvconn_read_cb(uv_stream_t *s, ssize_t nread, const uv_buf_t *buf)
{
	UvConn *c = (UvConn *) s;

	if (nread < 0) {		/* EOF or error */
		uvconn_close(c);
		return;
	}
	if (nread > 0 &&
	    protobuf_c_grpc2_conn_read(c->conn, (const uint8_t *) buf->base,
			    (size_t) nread) < 0)
		uvconn_close(c);
	/* Output is flushed by the check handle at the end of this iteration. */
}

static void
uvconn_handle_closed(uv_handle_t *h)
{
	UvConn *c = h->data;

	if (--c->handles_open > 0)
		return;
	{
		void (*cb)(void *) = c->on_closed;
		void *arg = c->on_closed_arg;
		*c->pprev = c->next;
		if (c->next != NULL)
			c->next->pprev = c->pprev;
		protobuf_c_grpc2_conn_free(c->conn);
		free(c);
		if (cb != NULL)
			cb(arg);
	}
}

/* Close the socket and free everything once libuv has released it. */
static void
uvconn_close(UvConn *c)
{
	if (c->closing)
		return;
	c->closing = 1;
	uv_read_stop((uv_stream_t *) &c->tcp);
	c->handles_open = 2;
	uv_close((uv_handle_t *) &c->tcp, uvconn_handle_closed);
	uv_close((uv_handle_t *) &c->timer, uvconn_handle_closed);
}

/* Call once per loop, before creating connections. */
static void
uvconn_loop_init(uv_loop_t *loop)
{
	uv_prepare_init(loop, &uvconn_prepare);
	uv_prepare_start(&uvconn_prepare, uvconn_prepare_cb);
	uv_unref((uv_handle_t *) &uvconn_prepare);
}

/* Wrap `conn` (takes ownership). The caller then accepts or connects
 * c->tcp, and calls uvconn_start(). */
static UvConn *
uvconn_new(uv_loop_t *loop, ProtobufCGrpc2Conn *conn)
{
	UvConn *c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->conn = conn;
	uv_tcp_init(loop, &c->tcp);
	uv_timer_init(loop, &c->timer);
	c->tcp.data = c;
	c->timer.data = c;
	c->next = uvconn_list;
	c->pprev = &uvconn_list;
	if (uvconn_list != NULL)
		uvconn_list->pprev = &c->next;
	uvconn_list = c;
	return c;
}

static void
uvconn_start(UvConn *c)
{
	uv_read_start((uv_stream_t *) &c->tcp, uvconn_alloc_cb, uvconn_read_cb);
}

#endif /* UV_GRPC2_H */
