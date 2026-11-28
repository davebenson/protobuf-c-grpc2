/*
 * GLib main-loop implementation of ProtobufCGrpc2IoSystem.
 *
 * The mapping onto a GMainContext:
 *   - each socket has one GSource, with its file descriptor added by
 *     g_source_add_unix_fd() while there is anything to wait for;
 *   - timers are GSources with a monotonic deadline (so no wall-clock jumps
 *     and no limit on the delay) that they report as the poll timeout;
 *   - deferred tasks (completion callbacks, deferred frees) are idle sources:
 *     one-shot, and the context does not block while one is pending;
 *   - iteration hooks are sources whose `prepare` function runs the hook. The
 *     context calls it at the start of every iteration, before it works out
 *     what to wait for, and the source has the highest priority so that
 *     nothing ready can keep it from running.
 *
 * `post` attaches an idle source from whichever thread calls it, which GLib
 * allows and which wakes the context.
 *
 * Objects are never freed from inside a callback: closing something removes
 * its source, closes its descriptor and queues a task that reports completion
 * and frees the memory, so no callback can arrive for a freed object.
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
#include <sys/types.h>
#include <unistd.h>

#include "protobuf-c-grpc2-glib.h"

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define EV_READ  1u
#define EV_WRITE 2u

enum { K_STREAM, K_LISTENER, K_CONNECT };

typedef struct Entry Entry;

/* The source of one descriptor. */
typedef struct {
	GSource base;
	Entry *e;
	gpointer tag;		/* from g_source_add_unix_fd(); NULL: not polled */
} FdSource;

struct Entry {
	int kind;
	int fd;			/* -1 once closed */
	unsigned watched;	/* EV_READ and/or EV_WRITE; 0: not polled */
	int closing;
	GMainContext *ctx;	/* not owned: the I/O system holds it */
	FdSource *src;		/* NULL until first needed, and once closed */
};

typedef struct {
	ProtobufCGrpc2IoSystem base;	/* must be first */
	GMainContext *ctx;
	int refs;
	int floating;		/* the creation reference is not yet claimed */
} GlibIo;

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

/* A timer is its own source: asleep (no deadline) until it is started. */
typedef struct {
	GSource base;
	gint64 deadline;	/* monotonic microseconds; -1: asleep */
	void (*callback)(void *);
	void *data;
} Timer;

typedef struct {
	GSource base;
	void (*hook)(void *);
	void *data;
} Hook;

/* ---- tasks ---- */

typedef struct {
	void (*fn)(void *);
	void *arg;
} Task;

static gboolean
task_run(gpointer data)
{
	Task *t = data;
	void (*fn)(void *) = t->fn;
	void *arg = t->arg;

	free(t);
	fn(arg);
	return G_SOURCE_REMOVE;
}

static int
task_post(GMainContext *ctx, void (*fn)(void *), void *arg)
{
	Task *t = malloc(sizeof(*t));
	GSource *s;

	if (t == NULL)
		return -1;
	t->fn = fn;
	t->arg = arg;
	s = g_idle_source_new();
	g_source_set_priority(s, G_PRIORITY_DEFAULT);
	g_source_set_callback(s, task_run, t, NULL);
	g_source_attach(s, ctx);
	g_source_unref(s);
	return 0;
}

/* ---- descriptor sources ---- */

static void entry_event(Entry *e, unsigned events);

static gboolean
fdsrc_check(GSource *s)
{
	FdSource *f = (FdSource *) s;

	return f->tag != NULL && g_source_query_unix_fd(s, f->tag) != 0;
}

static gboolean
fdsrc_dispatch(GSource *s, GSourceFunc callback, gpointer user_data)
{
	FdSource *f = (FdSource *) s;
	Entry *e = f->e;
	GIOCondition rev = f->tag != NULL ? g_source_query_unix_fd(s, f->tag) : 0;
	unsigned ev = 0;
	(void) callback;
	(void) user_data;

	/* an error or hang-up is delivered to whatever was asked for, so that the
	 * read or write that fails reports it */
	if (rev & (G_IO_IN | G_IO_HUP | G_IO_ERR))
		ev |= e->watched & EV_READ;
	if (rev & (G_IO_OUT | G_IO_HUP | G_IO_ERR))
		ev |= e->watched & EV_WRITE;
	if (ev != 0 && !e->closing)
		entry_event(e, ev);
	return G_SOURCE_CONTINUE;
}

static GSourceFuncs fdsrc_funcs = { NULL, fdsrc_check, fdsrc_dispatch, NULL,
				    NULL, NULL };

static void
entry_init(Entry *e, GMainContext *ctx, int kind, int fd)
{
	e->kind = kind;
	e->fd = fd;
	e->watched = 0;
	e->closing = 0;
	e->ctx = ctx;
	e->src = NULL;
}

static GIOCondition
conditions(unsigned events)
{
	return (GIOCondition) (((events & EV_READ) ? G_IO_IN : 0) |
			       ((events & EV_WRITE) ? G_IO_OUT : 0));
}

/* Watch for `events` (EV_READ and/or EV_WRITE), or stop watching with 0. A
 * descriptor that is not wanted is not polled at all: a hang-up could
 * otherwise wake the loop again and again with nobody to handle it. */
static void
entry_watch(Entry *e, unsigned events)
{
	FdSource *f;

	if (e->closing || e->fd < 0 || events == e->watched)
		return;
	f = e->src;
	if (events == 0) {
		if (f != NULL && f->tag != NULL) {
			g_source_remove_unix_fd(&f->base, f->tag);
			f->tag = NULL;
		}
		e->watched = 0;
		return;
	}
	if (f == NULL) {
		f = (FdSource *) g_source_new(&fdsrc_funcs, sizeof(FdSource));
		f->e = e;
		g_source_attach(&f->base, e->ctx);
		e->src = f;
	}
	if (f->tag == NULL)
		f->tag = g_source_add_unix_fd(&f->base, e->fd, conditions(events));
	else
		g_source_modify_unix_fd(&f->base, f->tag, conditions(events));
	e->watched = events;
}

/* Take the entry out of the context and close its descriptor. The memory
 * stays valid until the task that frees it runs. */
static void
entry_close(Entry *e)
{
	if (e->closing)
		return;
	e->closing = 1;
	e->watched = 0;
	if (e->src != NULL) {
		g_source_destroy(&e->src->base);
		g_source_unref(&e->src->base);
		e->src = NULL;
	}
	if (e->fd >= 0) {
		close(e->fd);
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
stream_new(GMainContext *ctx, int fd)
{
	Stream *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return NULL;
	entry_init(&s->e, ctx, K_STREAM, fd);
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
glib_stream_start(ProtobufCGrpc2IoSystem *io, void *stream,
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
glib_stream_write(ProtobufCGrpc2IoSystem *io, void *stream,
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
glib_stream_close(ProtobufCGrpc2IoSystem *io, void *stream)
{
	Stream *s = stream;
	(void) io;

	if (s->e.closing)
		return;
	if (s->woff < s->wlen)
		stream_flush(s);	/* best effort */
	entry_close(&s->e);
	if (task_post(s->e.ctx, stream_closed_task, s) != 0)
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
glib_listen(ProtobufCGrpc2IoSystem *io, const char *host, unsigned port,
	    void (*accepted)(void *, void *), void *accepted_data, void **out)
{
	GMainContext *ctx = ((GlibIo *) io)->ctx;
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
	entry_init(&l->e, ctx, K_LISTENER, fd);
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
glib_unlisten(ProtobufCGrpc2IoSystem *io, void *listener)
{
	Listener *l = listener;
	(void) io;

	if (l->e.closing)
		return;
	entry_close(&l->e);
	if (task_post(l->e.ctx, listener_free_task, l) != 0)
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
		    (s = stream_new(l->e.ctx, fd)) == NULL) {
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
	if (task_post(c->e.ctx, connect_report_task, c) != 0)
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
			entry_init(&c->e, c->e.ctx, K_CONNECT, fd);
			entry_watch(&c->e, EV_WRITE | EV_READ);
			return;
		}
		c->last_error = errno;
		close(fd);
	}
	connect_fail(c);
}

static int
glib_connect(ProtobufCGrpc2IoSystem *io, const char *host, unsigned port,
	     void (*connected)(void *, void *, int), void *data, void **handle)
{
	GMainContext *ctx = ((GlibIo *) io)->ctx;
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
	entry_init(&c->e, ctx, K_CONNECT, -1);
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
glib_cancel_connect(ProtobufCGrpc2IoSystem *io, void *handle)
{
	Connect *c = handle;
	(void) io;

	if (c->cancelled)
		return;
	c->cancelled = 1;
	if (c->e.fd >= 0) {
		/* in flight: nobody else will report it */
		entry_close(&c->e);
		if (task_post(c->e.ctx, connect_free_task, c) != 0)
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
	c->e.fd = -1;		/* keep entry_close from closing it */
	entry_close(&c->e);
	cb = c->connected;
	data = c->data;
	s = stream_new(c->e.ctx, fd);
	if (s == NULL) {
		close(fd);
		c->last_error = ENOMEM;
		connect_fail(c);
		return;
	}
	connect_free(c);
	cb(data, s, 0);
}

/* ---- the source callback ---- */

static void
entry_event(Entry *e, unsigned events)
{
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

/* Not g_source_set_ready_time(): every change of it wakes the context up, and
 * the core restarts its timers from the iteration hook, which would keep the
 * loop from ever sleeping. The deadline is kept here and turned into a poll
 * timeout by `prepare`; the hook has a higher priority than the timers, so a
 * deadline it sets is seen in the same iteration. */
static gboolean
timer_prepare(GSource *s, gint *timeout)
{
	Timer *t = (Timer *) s;
	gint64 diff;

	if (t->deadline < 0) {
		*timeout = -1;
		return FALSE;
	}
	diff = t->deadline - g_get_monotonic_time();
	if (diff <= 0) {
		*timeout = 0;
		return TRUE;
	}
	diff = (diff + 999) / 1000;
	*timeout = diff > G_MAXINT ? G_MAXINT : (gint) diff;
	return FALSE;
}

static gboolean
timer_check(GSource *s)
{
	Timer *t = (Timer *) s;

	return t->deadline >= 0 && g_get_monotonic_time() >= t->deadline;
}

static gboolean
timer_dispatch(GSource *s, GSourceFunc callback, gpointer user_data)
{
	Timer *t = (Timer *) s;
	void (*cb)(void *) = t->callback;
	void *data = t->data;
	(void) callback;
	(void) user_data;

	/* one-shot: asleep again unless the callback starts it */
	t->deadline = -1;
	cb(data);
	return G_SOURCE_CONTINUE;
}

static GSourceFuncs timer_funcs = { timer_prepare, timer_check, timer_dispatch,
				    NULL, NULL, NULL };

static void *
glib_timer_new(ProtobufCGrpc2IoSystem *io, void (*cb)(void *), void *data)
{
	Timer *t = (Timer *) g_source_new(&timer_funcs, sizeof(Timer));

	t->callback = cb;
	t->data = data;
	t->deadline = -1;
	g_source_attach(&t->base, ((GlibIo *) io)->ctx);
	return t;
}

static void
glib_timer_stop(ProtobufCGrpc2IoSystem *io, void *timer)
{
	(void) io;
	((Timer *) timer)->deadline = -1;
}

static void
glib_timer_start(ProtobufCGrpc2IoSystem *io, void *timer, uint64_t ms)
{
	gint64 now = g_get_monotonic_time();
	(void) io;

	/* A timer started from a timer callback with 0 ms is due at once but
	 * runs in the next iteration: one that restarts itself cannot starve
	 * the loop. */
	if (ms > (uint64_t) ((G_MAXINT64 - now) / 1000))
		ms = (uint64_t) ((G_MAXINT64 - now) / 1000);
	((Timer *) timer)->deadline = now + (gint64) ms * 1000;
}

static void
glib_timer_free(ProtobufCGrpc2IoSystem *io, void *timer)
{
	Timer *t = timer;
	(void) io;

	g_source_destroy(&t->base);
	g_source_unref(&t->base);
}

/* ---- hooks ---- */

static gboolean
hook_prepare(GSource *s, gint *timeout)
{
	Hook *h = (Hook *) s;

	*timeout = -1;
	h->hook(h->data);
	return FALSE;		/* never ready itself */
}

static gboolean
hook_dispatch(GSource *s, GSourceFunc callback, gpointer user_data)
{
	(void) s;
	(void) callback;
	(void) user_data;
	return G_SOURCE_CONTINUE;
}

static GSourceFuncs hook_funcs = { hook_prepare, NULL, hook_dispatch, NULL, NULL, NULL };

static void *
glib_add_hook(ProtobufCGrpc2IoSystem *io, void (*hook)(void *), void *data)
{
	Hook *h = (Hook *) g_source_new(&hook_funcs, sizeof(Hook));

	h->hook = hook;
	h->data = data;
	/* prepared before everything else, so that a ready source of higher
	 * priority cannot keep it from running */
	g_source_set_priority(&h->base, G_PRIORITY_HIGH);
	g_source_attach(&h->base, ((GlibIo *) io)->ctx);
	return h;
}

static void
glib_remove_hook(ProtobufCGrpc2IoSystem *io, void *hook)
{
	Hook *h = hook;
	(void) io;

	/* safe from inside the hook itself: the context holds a reference to
	 * the source while it prepares it */
	g_source_destroy(&h->base);
	g_source_unref(&h->base);
}

/* ---- posting from other threads ---- */

/* Sources may be attached to a context from any thread, which wakes it up, and
 * idle sources of one priority run in the order they were attached. */
static int
glib_post(ProtobufCGrpc2IoSystem *io, void (*fn)(void *), void *data)
{
	return task_post(((GlibIo *) io)->ctx, fn, data);
}

/* ---- the I/O system ---- */

/* Like g_object_ref_sink(): the first reference taken claims the creation
 * reference instead of adding one. */
static void
glib_ref(ProtobufCGrpc2IoSystem *io)
{
	GlibIo *p = (GlibIo *) io;

	if (p->floating)
		p->floating = 0;
	else
		p->refs++;
}

static void
glib_destroy(ProtobufCGrpc2IoSystem *io)
{
	GlibIo *p = (GlibIo *) io;

	if (--p->refs > 0)
		return;
	g_main_context_unref(p->ctx);
	free(p);
}

ProtobufCGrpc2IoSystem *
protobuf_c_grpc2_io_system_new_glib(GMainContext *context)
{
	GlibIo *io = calloc(1, sizeof(*io));

	if (io == NULL)
		return NULL;
	io->ctx = g_main_context_ref(context != NULL
				     ? context : g_main_context_default());
	io->base.listen = glib_listen;
	io->base.unlisten = glib_unlisten;
	io->base.connect = glib_connect;
	io->base.cancel_connect = glib_cancel_connect;
	io->base.stream_start = glib_stream_start;
	io->base.stream_write = glib_stream_write;
	io->base.stream_close = glib_stream_close;
	io->base.timer_new = glib_timer_new;
	io->base.timer_start = glib_timer_start;
	io->base.timer_stop = glib_timer_stop;
	io->base.timer_free = glib_timer_free;
	io->base.add_iteration_hook = glib_add_hook;
	io->base.remove_iteration_hook = glib_remove_hook;
	io->base.destroy = glib_destroy;
	io->base.ref = glib_ref;
	io->base.post = glib_post;
	io->refs = 1;
	io->floating = 1;
	return &io->base;
}
