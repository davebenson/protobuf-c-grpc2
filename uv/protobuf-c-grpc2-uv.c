/* libuv implementation of ProtobufCGrpc2IoSystem. */
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2-uv.h"

typedef struct {
	ProtobufCGrpc2IoSystem base;	/* must be first */
	uv_loop_t *loop;
} UvIo;

typedef struct {
	uv_tcp_t tcp;			/* must be first */
	ProtobufCGrpc2IoHandler handler;
	int started;
	char rbuf[16384];
} UvStream;

typedef struct {
	uv_tcp_t tcp;			/* must be first */
	UvIo *io;
	void (*accepted)(void *data, void *stream);
	void *accepted_data;
} UvListener;

typedef struct {
	uv_timer_t timer;		/* must be first */
	void (*callback)(void *);
	void *data;
} UvTimer;

typedef struct {
	uv_prepare_t prepare;		/* must be first */
	void (*hook)(void *);
	void *data;
} UvHook;

static void
free_handle(uv_handle_t *h)
{
	free(h);
}

/* ---- streams ---- */

static void
stream_closed(uv_handle_t *h)
{
	UvStream *s = (UvStream *) h;
	void (*closed)(void *) = s->handler.closed;
	void *data = s->handler.data;
	int started = s->started;

	free(s);
	if (started && closed != NULL)
		closed(data);
}

static void
stream_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf)
{
	UvStream *s = (UvStream *) h;
	(void) suggested;
	*buf = uv_buf_init(s->rbuf, sizeof s->rbuf);
}

static void
stream_read(uv_stream_t *h, ssize_t nread, const uv_buf_t *buf)
{
	UvStream *s = (UvStream *) h;

	if (nread < 0) {
		uv_read_stop(h);
		s->handler.eof(s->handler.data, nread == UV_EOF ? 0 : (int) nread);
	} else if (nread > 0) {
		s->handler.received(s->handler.data, (const uint8_t *) buf->base,
				    (size_t) nread);
	}
}

static int
uv_stream_start_(ProtobufCGrpc2IoSystem *io, void *stream,
		 const ProtobufCGrpc2IoHandler *handler)
{
	UvStream *s = stream;
	(void) io;

	s->handler = *handler;
	s->started = 1;
	return uv_read_start((uv_stream_t *) &s->tcp, stream_alloc, stream_read) != 0
		? -1 : 0;
}

static void
write_done(uv_write_t *req, int status)
{
	(void) status;
	free(req);
}

static int
uv_stream_write_(ProtobufCGrpc2IoSystem *io, void *stream, const uint8_t *data,
		 size_t len)
{
	UvStream *s = stream;
	uv_write_t *req = malloc(sizeof(*req) + len);
	uv_buf_t b;
	(void) io;

	if (req == NULL)
		return -1;
	memcpy(req + 1, data, len);
	b = uv_buf_init((char *) (req + 1), (unsigned) len);
	if (uv_write(req, (uv_stream_t *) &s->tcp, &b, 1, write_done) != 0) {
		free(req);
		return -1;
	}
	return 0;
}

static void
uv_stream_close_(ProtobufCGrpc2IoSystem *io, void *stream)
{
	UvStream *s = stream;
	(void) io;

	if (s->started)
		uv_read_stop((uv_stream_t *) &s->tcp);
	uv_close((uv_handle_t *) &s->tcp, stream_closed);
}

/* ---- listening ---- */

static void
on_connection(uv_stream_t *server, int status)
{
	UvListener *l = (UvListener *) server;
	UvStream *s;

	if (status < 0)
		return;
	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return;
	uv_tcp_init(l->io->loop, &s->tcp);
	if (uv_accept(server, (uv_stream_t *) &s->tcp) != 0) {
		uv_close((uv_handle_t *) &s->tcp, stream_closed);
		return;
	}
	l->accepted(l->accepted_data, s);
}

static int
uv_listen_(ProtobufCGrpc2IoSystem *io, const char *host, unsigned port,
	   void (*accepted)(void *, void *), void *accepted_data, void **out)
{
	UvIo *uvio = (UvIo *) io;
	UvListener *l;
	struct sockaddr_storage addr;
	int rv;

	if (uv_ip4_addr(host, (int) port, (struct sockaddr_in *) &addr) != 0 &&
	    uv_ip6_addr(host, (int) port, (struct sockaddr_in6 *) &addr) != 0)
		return -1;
	l = calloc(1, sizeof(*l));
	if (l == NULL)
		return -1;
	l->io = uvio;
	l->accepted = accepted;
	l->accepted_data = accepted_data;
	uv_tcp_init(uvio->loop, &l->tcp);
	rv = uv_tcp_bind(&l->tcp, (const struct sockaddr *) &addr, 0);
	if (rv == 0)
		rv = uv_listen((uv_stream_t *) &l->tcp, 128, on_connection);
	if (rv != 0) {
		uv_close((uv_handle_t *) &l->tcp, free_handle);
		return -1;
	}
	*out = l;
	return 0;
}

static void
uv_unlisten_(ProtobufCGrpc2IoSystem *io, void *listener)
{
	UvListener *l = listener;
	(void) io;
	uv_close((uv_handle_t *) &l->tcp, free_handle);
}

/* ---- timers and hooks ---- */

static void
timer_fired(uv_timer_t *t)
{
	UvTimer *tm = (UvTimer *) t;
	tm->callback(tm->data);
}

static void *
uv_timer_new_(ProtobufCGrpc2IoSystem *io, void (*cb)(void *), void *data)
{
	UvTimer *t = calloc(1, sizeof(*t));
	if (t == NULL)
		return NULL;
	t->callback = cb;
	t->data = data;
	uv_timer_init(((UvIo *) io)->loop, &t->timer);
	return t;
}

static void
uv_timer_start_(ProtobufCGrpc2IoSystem *io, void *timer, uint64_t ms)
{
	(void) io;
	uv_timer_start(timer, timer_fired, ms, 0);
}

static void
uv_timer_stop_(ProtobufCGrpc2IoSystem *io, void *timer)
{
	(void) io;
	uv_timer_stop(timer);
}

static void
uv_timer_free_(ProtobufCGrpc2IoSystem *io, void *timer)
{
	(void) io;
	uv_timer_stop(timer);
	uv_close(timer, free_handle);
}

/*
 * A prepare handle runs after the timer callbacks of an iteration and before
 * the loop blocks, so output produced by timers is flushed before waiting.
 * (A check handle would run only after the next wakeup.)
 */
static void
hook_fired(uv_prepare_t *c)
{
	UvHook *h = (UvHook *) c;
	h->hook(h->data);
}

static void *
uv_add_hook_(ProtobufCGrpc2IoSystem *io, void (*hook)(void *), void *data)
{
	UvHook *h = calloc(1, sizeof(*h));
	if (h == NULL)
		return NULL;
	h->hook = hook;
	h->data = data;
	uv_prepare_init(((UvIo *) io)->loop, &h->prepare);
	uv_prepare_start(&h->prepare, hook_fired);
	uv_unref((uv_handle_t *) &h->prepare);	/* must not keep the loop alive */
	return h;
}

static void
uv_remove_hook_(ProtobufCGrpc2IoSystem *io, void *hook)
{
	UvHook *h = hook;
	(void) io;
	uv_prepare_stop(&h->prepare);
	uv_close((uv_handle_t *) &h->prepare, free_handle);
}

static void
uv_destroy_(ProtobufCGrpc2IoSystem *io)
{
	free(io);
}

ProtobufCGrpc2IoSystem *
protobuf_c_grpc2_io_system_new_uv(uv_loop_t *loop)
{
	UvIo *io = calloc(1, sizeof(*io));

	if (io == NULL)
		return NULL;
	io->loop = loop;
	io->base.listen = uv_listen_;
	io->base.unlisten = uv_unlisten_;
	io->base.stream_start = uv_stream_start_;
	io->base.stream_write = uv_stream_write_;
	io->base.stream_close = uv_stream_close_;
	io->base.timer_new = uv_timer_new_;
	io->base.timer_start = uv_timer_start_;
	io->base.timer_stop = uv_timer_stop_;
	io->base.timer_free = uv_timer_free_;
	io->base.add_iteration_hook = uv_add_hook_;
	io->base.remove_iteration_hook = uv_remove_hook_;
	io->base.destroy = uv_destroy_;
	return &io->base;
}

int
protobuf_c_grpc2_server_builder_use_uv(ProtobufCGrpc2ServerBuilder *builder,
				       uv_loop_t *loop)
{
	ProtobufCGrpc2IoSystem *io = protobuf_c_grpc2_io_system_new_uv(loop);

	if (io == NULL)
		return -1;
	if (protobuf_c_grpc2_server_builder_add_io_system(builder, io) != 0) {
		io->destroy(io);
		return -1;
	}
	return 0;
}
