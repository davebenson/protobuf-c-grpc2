/*
 * ProtobufCRPCDispatch implementation of ProtobufCGrpc2IoSystem.
 *
 * The mapping onto the dispatch:
 *   - sockets are watched with protobuf_c_rpc_dispatch_watch_fd();
 *   - timers are dispatch timers (one-shot, like ours), started at an
 *     absolute time computed here, because the dispatch's "millis" variant
 *     counts from the start of its last iteration;
 *   - deferred tasks (completion callbacks, deferred frees) are idle
 *     functions: one-shot, and the dispatch polls with timeout 0 while one is
 *     pending;
 *   - iteration hooks are prepare functions, which the dispatch runs right
 *     before it works out what to wait for.
 *
 * Objects are never freed from inside a callback: closing something unwatches
 * it, closes its descriptor and queues a task that reports completion and
 * frees the memory, so no callback can arrive for a freed object.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "protobuf-c-grpc2-dispatch.h"

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define EV_READ  PROTOBUF_C_RPC_EVENT_READABLE
#define EV_WRITE PROTOBUF_C_RPC_EVENT_WRITABLE

enum { K_STREAM, K_LISTENER, K_CONNECT };

typedef struct {
	int kind;
	int fd;			/* -1 once closed */
	unsigned watched;	/* the events the dispatch watches for; 0: none */
	int closing;
	ProtobufCRPCDispatch *d;
} Entry;

typedef struct {
	ProtobufCGrpc2IoSystem base;	/* must be first */
	ProtobufCRPCDispatch *d;
	int refs;
	int floating;		/* the creation reference is not yet claimed */
} DispIo;

typedef struct {
	Entry e;			/* must be first */
	ProtobufCGrpc2IoHandler handler;
	int started;
	int eof_seen;
	uint8_t *wbuf;			/* unsent output */
	size_t wlen, woff, wcap;
	uint8_t rbuf[16384];
} Stream;

typedef struct {
	Entry e;			/* must be first */
	void (*accepted)(void *, void *);
	void *accepted_data;
} Listener;

typedef struct {
	Entry e;			/* must be first; fd: current attempt */
	struct addrinfo *res, *cur;
	unsigned port;
	void (*connected)(void *, void *, int);
	void *data;
	int cancelled;
	int last_error;
} Connect;

typedef struct {
	ProtobufCRPCDispatch *d;
	ProtobufCRPCDispatchTimer *t;	/* NULL when not active */
	void (*callback)(void *);
	void *data;
} Timer;

typedef struct {
	ProtobufCRPCDispatchPrepare *p;
	void (*hook)(void *);
	void *data;
} Hook;

/* ---- tasks ---- */

typedef struct {
	void (*fn)(void *);
	void *arg;
} Task;

static void
task_run(ProtobufCRPCDispatch *d, void *data)
{
	Task *t = data;
	void (*fn)(void *) = t->fn;
	void *arg = t->arg;
	(void) d;

	free(t);
	fn(arg);
}

static int
task_post(ProtobufCRPCDispatch *d, void (*fn)(void *), void *arg)
{
	Task *t = malloc(sizeof(*t));

	if (t == NULL)
		return -1;
	t->fn = fn;
	t->arg = arg;
	protobuf_c_rpc_dispatch_add_idle(d, task_run, t);
	return 0;
}

/* ---- entries ---- */

static void entry_event(int fd, unsigned events, void *data);

static void
entry_init(Entry *e, ProtobufCRPCDispatch *d, int kind, int fd)
{
	e->kind = kind;
	e->fd = fd;
	e->watched = 0;
	e->closing = 0;
	e->d = d;
}

/* Watch for `events` (READ and/or WRITE), or stop watching with 0. */
static void
entry_watch(Entry *e, unsigned events)
{
	if (e->closing || e->fd < 0 || events == e->watched)
		return;
	if (events == 0)
		protobuf_c_rpc_dispatch_watch_fd(e->d, e->fd, 0, NULL, NULL);
	else
		protobuf_c_rpc_dispatch_watch_fd(e->d, e->fd, events,
						 entry_event, e);
	e->watched = events;
}

/* Take the entry out of the dispatch and close its descriptor. The memory
 * stays valid until the task that frees it runs. */
static void
entry_close(Entry *e)
{
	if (e->closing)
		return;
	e->closing = 1;
	e->watched = 0;
	if (e->fd >= 0) {
		protobuf_c_rpc_dispatch_close_fd(e->d, e->fd);
		e->fd = -1;
	}
}

static int
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);

	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
		return -1;
	fl = fcntl(fd, F_GETFD, 0);
	if (fl >= 0)
		fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
	return 0;
}

static void
socket_setup(int fd)
{
	int one = 1;

#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

/* ---- streams ---- */

static Stream *
stream_new(ProtobufCRPCDispatch *d, int fd)
{
	Stream *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return NULL;
	entry_init(&s->e, d, K_STREAM, fd);
	return s;
}

static void
stream_update_events(Stream *s)
{
	entry_watch(&s->e, ((s->started && !s->eof_seen) ? EV_READ : 0) |
			   (s->woff < s->wlen ? EV_WRITE : 0));
}

/* 0: all sent, 1: would block, -1: failed */
static int
stream_flush(Stream *s)
{
	while (s->woff < s->wlen) {
		ssize_t n = send(s->e.fd, s->wbuf + s->woff, s->wlen - s->woff,
				 SEND_FLAGS);

		if (n > 0) {
			s->woff += (size_t) n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return 1;
		} else {
			s->wlen = s->woff = 0;
			return -1;
		}
	}
	s->wlen = s->woff = 0;
	return 0;
}

static int
disp_stream_start(ProtobufCGrpc2IoSystem *io, void *stream,
		  const ProtobufCGrpc2IoHandler *handler)
{
	Stream *s = stream;
	(void) io;

	if (s->e.closing)
		return -1;
	s->handler = *handler;
	s->started = 1;		/* from now on `closed` is reported */
	stream_update_events(s);
	return 0;
}

static int
disp_stream_write(ProtobufCGrpc2IoSystem *io, void *stream,
		  const uint8_t *data, size_t len)
{
	Stream *s = stream;
	(void) io;

	if (s->e.closing)
		return -1;
	if (len == 0)
		return 0;
	if (s->woff == s->wlen) {
		/* nothing queued: write directly, queue what does not fit */
		while (len > 0) {
			ssize_t n = send(s->e.fd, data, len, SEND_FLAGS);

			if (n > 0) {
				data += n;
				len -= (size_t) n;
			} else if (n < 0 && errno == EINTR) {
				continue;
			} else if (n < 0 && (errno == EAGAIN ||
					     errno == EWOULDBLOCK)) {
				break;
			} else {
				return -1;
			}
		}
		if (len == 0)
			return 0;
	}
	if (s->wlen + len > s->wcap) {
		size_t cap = s->wcap ? s->wcap : 4096;
		uint8_t *nb;

		if (s->woff > 0) {	/* reclaim the sent prefix first */
			memmove(s->wbuf, s->wbuf + s->woff, s->wlen - s->woff);
			s->wlen -= s->woff;
			s->woff = 0;
		}
		while (cap < s->wlen + len)
			cap *= 2;
		nb = realloc(s->wbuf, cap);
		if (nb == NULL)
			return -1;
		s->wbuf = nb;
		s->wcap = cap;
	}
	memcpy(s->wbuf + s->wlen, data, len);
	s->wlen += len;
	stream_update_events(s);
	return 0;
}

static void
stream_closed_task(void *arg)
{
	Stream *s = arg;
	void (*closed)(void *) = s->handler.closed;
	void *data = s->handler.data;
	int started = s->started;

	free(s->wbuf);
	free(s);
	if (started && closed != NULL)
		closed(data);
}

static void
disp_stream_close(ProtobufCGrpc2IoSystem *io, void *stream)
{
	Stream *s = stream;
	(void) io;

	if (s->e.closing)
		return;
	if (s->woff < s->wlen)
		stream_flush(s);	/* best effort */
	entry_close(&s->e);
	if (task_post(s->e.d, stream_closed_task, s) != 0)
		abort();		/* cannot report completion */
}

static void
stream_dispatch(Stream *s, unsigned events)
{
	if ((events & EV_WRITE) && s->woff < s->wlen) {
		stream_flush(s);	/* on failure the read side reports it */
		stream_update_events(s);
	}
	if (s->e.closing)
		return;
	if ((events & EV_READ) && s->started && !s->eof_seen) {
		ssize_t n = read(s->e.fd, s->rbuf, sizeof s->rbuf);

		if (n > 0) {
			s->handler.received(s->handler.data, s->rbuf, (size_t) n);
		} else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
				      errno != EINTR)) {
			int err = n == 0 ? 0 : errno;

			s->eof_seen = 1;
			stream_update_events(s);
			s->handler.eof(s->handler.data, err);
		}
	}
	if (!s->e.closing)
		stream_update_events(s);
}

/* ---- listening ---- */

static int
disp_listen(ProtobufCGrpc2IoSystem *io, const char *host, unsigned port,
	    void (*accepted)(void *, void *), void *accepted_data, void **out)
{
	ProtobufCRPCDispatch *d = ((DispIo *) io)->d;
	struct sockaddr_storage addr;
	socklen_t alen;
	Listener *l;
	int fd, one = 1;

	memset(&addr, 0, sizeof addr);
	if (port > 65535)
		return -1;
	if (inet_pton(AF_INET, host, &((struct sockaddr_in *) &addr)->sin_addr) == 1) {
		struct sockaddr_in *a = (struct sockaddr_in *) &addr;

		a->sin_family = AF_INET;
		a->sin_port = htons((uint16_t) port);
		alen = sizeof *a;
	} else if (inet_pton(AF_INET6, host,
			     &((struct sockaddr_in6 *) &addr)->sin6_addr) == 1) {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *) &addr;

		a->sin6_family = AF_INET6;
		a->sin6_port = htons((uint16_t) port);
		alen = sizeof *a;
	} else {
		return -1;
	}
	fd = socket(addr.ss_family, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (set_nonblock(fd) != 0 ||
	    bind(fd, (struct sockaddr *) &addr, alen) != 0 ||
	    listen(fd, 128) != 0) {
		close(fd);
		return -1;
	}
	l = calloc(1, sizeof(*l));
	if (l == NULL) {
		close(fd);
		return -1;
	}
	l->accepted = accepted;
	l->accepted_data = accepted_data;
	entry_init(&l->e, d, K_LISTENER, fd);
	entry_watch(&l->e, EV_READ);
	*out = l;
	return 0;
}

static void
listener_free_task(void *arg)
{
	free(arg);
}

static void
disp_unlisten(ProtobufCGrpc2IoSystem *io, void *listener)
{
	Listener *l = listener;
	(void) io;

	if (l->e.closing)
		return;
	entry_close(&l->e);
	if (task_post(l->e.d, listener_free_task, l) != 0)
		abort();
}

static void
listener_dispatch(Listener *l)
{
	int i;

	for (i = 0; i < 64 && !l->e.closing; i++) {
		int fd = accept(l->e.fd, NULL, NULL);
		Stream *s;

		if (fd < 0)
			break;		/* EAGAIN, or nothing we can do */
		if (set_nonblock(fd) != 0 ||
		    (s = stream_new(l->e.d, fd)) == NULL) {
			close(fd);
			continue;
		}
		socket_setup(fd);
		l->accepted(l->accepted_data, s);
	}
}

/* ---- connecting ---- */

static void
connect_free(Connect *c)
{
	if (c->res != NULL)
		freeaddrinfo(c->res);
	free(c);
}

static void
connect_free_task(void *arg)
{
	connect_free(arg);
}

static void
connect_report_task(void *arg)
{
	Connect *c = arg;
	void (*cb)(void *, void *, int) = c->connected;
	void *data = c->data;
	int err = c->last_error ? c->last_error : EIO;
	int cancelled = c->cancelled;

	connect_free(c);
	if (!cancelled)
		cb(data, NULL, err);
}

static void connect_try_next(Connect *c);

/* every address failed (or none resolved): report from a task, never from
 * inside the call that started or continued the attempt */
static void
connect_fail(Connect *c)
{
	if (task_post(c->e.d, connect_report_task, c) != 0)
		abort();
}

static void
connect_try_next(Connect *c)
{
	for (; c->cur != NULL; c->cur = c->cur->ai_next) {
		struct addrinfo *a = c->cur;
		int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		int rv;

		if (fd < 0) {
			c->last_error = errno;
			continue;
		}
		if (set_nonblock(fd) != 0) {
			c->last_error = errno;
			close(fd);
			continue;
		}
		socket_setup(fd);
		rv = connect(fd, a->ai_addr, a->ai_addrlen);
		if (rv == 0 || errno == EINPROGRESS || errno == EINTR) {
			/* completion (even an immediate one) is seen as
			 * writability, from the loop; an error may show as
			 * either direction */
			entry_init(&c->e, c->e.d, K_CONNECT, fd);
			entry_watch(&c->e, EV_WRITE | EV_READ);
			return;
		}
		c->last_error = errno;
		close(fd);
	}
	connect_fail(c);
}

static int
disp_connect(ProtobufCGrpc2IoSystem *io, const char *host, unsigned port,
	     void (*connected)(void *, void *, int), void *data, void **handle)
{
	ProtobufCRPCDispatch *d = ((DispIo *) io)->d;
	struct addrinfo hints;
	char portstr[16];
	Connect *c;
	int rv;

	if (port > 65535)
		return -1;
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return -1;
	c->port = port;
	c->connected = connected;
	c->data = data;
	entry_init(&c->e, d, K_CONNECT, -1);
	snprintf(portstr, sizeof portstr, "%u", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	rv = getaddrinfo(host, portstr, &hints, &c->res);	/* may block */
	c->cur = c->res;
	if (rv != 0 || c->res == NULL) {
		/* resolver errors are reported as -1000 - EAI_*, apart from
		 * the errno values of connect failures */
		c->last_error = rv != 0 ? -1000 - (rv < 0 ? -rv : rv) : EINVAL;
		c->res = NULL;
		connect_fail(c);
	} else {
		connect_try_next(c);
	}
	*handle = c;
	return 0;
}

static void
disp_cancel_connect(ProtobufCGrpc2IoSystem *io, void *handle)
{
	Connect *c = handle;
	(void) io;

	if (c->cancelled)
		return;
	c->cancelled = 1;
	if (c->e.fd >= 0) {
		/* in flight: nobody else will report it */
		entry_close(&c->e);
		if (task_post(c->e.d, connect_free_task, c) != 0)
			abort();
	}
	/* otherwise a report task is already queued; it sees `cancelled` */
}

static void
connect_dispatch(Connect *c)
{
	int err = 0;
	socklen_t elen = sizeof err;
	void (*cb)(void *, void *, int);
	void *data;
	Stream *s;
	int fd;

	if (getsockopt(c->e.fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0)
		err = errno;
	if (err != 0) {
		c->last_error = err;
		entry_close(&c->e);
		c->cur = c->cur->ai_next;
		connect_try_next(c);	/* re-initialises the entry */
		return;
	}
	/* connected: the descriptor becomes a stream */
	entry_watch(&c->e, 0);
	fd = c->e.fd;
	c->e.fd = -1;
	entry_close(&c->e);
	cb = c->connected;
	data = c->data;
	s = stream_new(c->e.d, fd);
	if (s == NULL) {
		close(fd);
		c->last_error = ENOMEM;
		connect_fail(c);
		return;
	}
	connect_free(c);
	cb(data, s, 0);
}

/* ---- the watch callback ---- */

static void
entry_event(int fd, unsigned events, void *data)
{
	Entry *e = data;
	(void) fd;

	if (e->closing)
		return;
	switch (e->kind) {
	case K_STREAM:
		stream_dispatch((Stream *) e, events);
		break;
	case K_LISTENER:
		listener_dispatch((Listener *) e);
		break;
	case K_CONNECT:
		connect_dispatch((Connect *) e);
		break;
	}
}

/* ---- timers ---- */

static void
timer_fire(ProtobufCRPCDispatch *d, void *data)
{
	Timer *t = data;
	(void) d;

	t->t = NULL;		/* the dispatch is done with it */
	t->callback(t->data);
}

static void *
disp_timer_new(ProtobufCGrpc2IoSystem *io, void (*cb)(void *), void *data)
{
	Timer *t = calloc(1, sizeof(*t));

	if (t == NULL)
		return NULL;
	t->d = ((DispIo *) io)->d;
	t->callback = cb;
	t->data = data;
	return t;
}

static void
disp_timer_stop(ProtobufCGrpc2IoSystem *io, void *timer)
{
	Timer *t = timer;
	(void) io;

	if (t->t != NULL) {
		protobuf_c_rpc_dispatch_remove_timer(t->t);
		t->t = NULL;
	}
}

static void
disp_timer_start(ProtobufCGrpc2IoSystem *io, void *timer, uint64_t ms)
{
	Timer *t = timer;
	struct timeval tv;
	uint64_t usec;

	disp_timer_stop(io, timer);
	/* An absolute time on the clock the dispatch uses. A timer started from
	 * a timer callback with 0 ms is due at once, but the dispatch compares
	 * against the time it started this iteration, so it runs in the next
	 * one: a timer that restarts itself cannot starve the loop. */
	gettimeofday(&tv, NULL);
	usec = (uint64_t) tv.tv_usec + (ms % 1000) * 1000;
	t->t = protobuf_c_rpc_dispatch_add_timer(
		t->d,
		(unsigned) ((uint64_t) tv.tv_sec + ms / 1000 + usec / 1000000),
		(unsigned) (usec % 1000000), timer_fire, t);
}

static void
disp_timer_free(ProtobufCGrpc2IoSystem *io, void *timer)
{
	disp_timer_stop(io, timer);
	free(timer);
}

/* ---- hooks ---- */

static void
hook_fire(ProtobufCRPCDispatch *d, void *data)
{
	Hook *h = data;
	(void) d;

	h->hook(h->data);
}

static void *
disp_add_hook(ProtobufCGrpc2IoSystem *io, void (*hook)(void *), void *data)
{
	Hook *h = calloc(1, sizeof(*h));

	if (h == NULL)
		return NULL;
	h->hook = hook;
	h->data = data;
	h->p = protobuf_c_rpc_dispatch_add_prepare(((DispIo *) io)->d,
						    hook_fire, h);
	return h;
}

static void
disp_remove_hook(ProtobufCGrpc2IoSystem *io, void *hook)
{
	Hook *h = hook;
	(void) io;

	/* safe from inside the hook itself: the dispatch only unlinks it
	 * when it has finished running them */
	protobuf_c_rpc_dispatch_remove_prepare(h->p);
	free(h);
}

/* ---- the I/O system ---- */

/* Like g_object_ref_sink(): the first reference taken claims the creation
 * reference instead of adding one. */
static void
disp_ref(ProtobufCGrpc2IoSystem *io)
{
	DispIo *p = (DispIo *) io;

	if (p->floating)
		p->floating = 0;
	else
		p->refs++;
}

static void
disp_destroy(ProtobufCGrpc2IoSystem *io)
{
	DispIo *p = (DispIo *) io;

	if (--p->refs > 0)
		return;
	free(p);
}

ProtobufCGrpc2IoSystem *
protobuf_c_grpc2_io_system_new_dispatch(const ProtobufCGrpc2DispatchOptions *options)
{
	ProtobufCGrpc2DispatchOptions o;
	DispIo *io;

	if (protobuf_c_grpc2_options_copy(&o, sizeof o, options) != 0)
		return NULL;
	io = calloc(1, sizeof(*io));

	if (io == NULL)
		return NULL;
	io->d = o.dispatch != NULL ? o.dispatch
				    : protobuf_c_rpc_dispatch_default();
	io->base.listen = disp_listen;
	io->base.unlisten = disp_unlisten;
	io->base.connect = disp_connect;
	io->base.cancel_connect = disp_cancel_connect;
	io->base.stream_start = disp_stream_start;
	io->base.stream_write = disp_stream_write;
	io->base.stream_close = disp_stream_close;
	io->base.timer_new = disp_timer_new;
	io->base.timer_start = disp_timer_start;
	io->base.timer_stop = disp_timer_stop;
	io->base.timer_free = disp_timer_free;
	io->base.add_iteration_hook = disp_add_hook;
	io->base.remove_iteration_hook = disp_remove_hook;
	io->base.destroy = disp_destroy;
	io->base.ref = disp_ref;
	io->refs = 1;
	io->floating = 1;
	return &io->base;
}
