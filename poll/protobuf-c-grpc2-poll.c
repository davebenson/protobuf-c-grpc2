/*
 * poll(2) implementation of ProtobufCGrpc2IoSystem.
 *
 * One iteration of the loop is:
 *   1. run the deferred tasks (completion callbacks, deferred frees),
 *   2. run the expired timers,
 *   3. run the iteration hooks (where the core flushes its output),
 *   4. poll() for as long as the next timer allows (not at all while tasks
 *      are pending),
 *   5. dispatch the ready descriptors.
 * The loop is alive while it has a task pending, an active timer or an
 * active descriptor. Hooks do not keep it alive.
 *
 * Objects are never freed from inside a dispatch: closing something unlinks
 * it, closes its descriptor and queues a task that reports completion and
 * frees the memory. The array of descriptors being dispatched may therefore
 * hold entries that were closed by an earlier callback; they are skipped.
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
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "protobuf-c-grpc2-poll.h"

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

enum { K_STREAM, K_LISTENER, K_CONNECT };

typedef struct Entry Entry;
struct Entry {
	int kind;
	int fd;			/* -1 once closed */
	short events;		/* what to poll() for; 0: not polled */
	int closing;
	Entry *prev, *next;	/* the loop's list of live entries */
};

typedef struct Task Task;
struct Task {
	Task *next;
	void (*fn)(void *);
	void *arg;
};

typedef struct Timer Timer;
struct Timer {
	Timer *prev, *next;
	void (*callback)(void *);
	void *data;
	int active;
	uint64_t deadline;	/* ms, monotonic */
	uint64_t fired_pass;
};

typedef struct Hook Hook;
struct Hook {
	Hook *next;
	void (*hook)(void *);
	void *data;
	int removed;
};

struct ProtobufCGrpc2PollLoop {
	/* post(): the only thing other threads touch */
	pthread_mutex_t post_mutex;
	Task *posted, *posted_tail;
	int wake[2];		/* self-pipe: written by post(), polled here */

	Entry *entries;
	Timer *timers;
	Hook *hooks;
	Task *tasks, *tasks_tail;
	size_t n_objects;	/* everything not yet freed, for loop_close */
	int stopped;
	int in_hooks;
	uint64_t pass;
	struct pollfd *pfds;	/* scratch for one iteration */
	Entry **ents;
	size_t cap;
};

typedef struct {
	ProtobufCGrpc2IoSystem base;	/* must be first */
	ProtobufCGrpc2PollLoop *loop;
	int refs;
	int floating;		/* the creation reference is not yet claimed */
} PollIo;

typedef struct {
	Entry e;			/* must be first */
	ProtobufCGrpc2PollLoop *loop;
	ProtobufCGrpc2IoHandler handler;
	int started;
	int eof_seen;
	uint8_t *wbuf;			/* unsent output */
	size_t wlen, woff, wcap;
	uint8_t rbuf[16384];
} Stream;

typedef struct {
	Entry e;			/* must be first */
	ProtobufCGrpc2PollLoop *loop;
	void (*accepted)(void *, void *);
	void *accepted_data;
} Listener;

typedef struct {
	Entry e;			/* must be first; fd: current attempt */
	ProtobufCGrpc2PollLoop *loop;
	struct addrinfo *res, *cur;
	unsigned port;
	void (*connected)(void *, void *, int);
	void *data;
	int cancelled;
	int last_error;
} Connect;

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}

/* ---- tasks ---- */

static int
task_post(ProtobufCGrpc2PollLoop *loop, void (*fn)(void *), void *arg)
{
	Task *t = malloc(sizeof(*t));

	if (t == NULL)
		return -1;
	t->next = NULL;
	t->fn = fn;
	t->arg = arg;
	if (loop->tasks_tail != NULL)
		loop->tasks_tail->next = t;
	else
		loop->tasks = t;
	loop->tasks_tail = t;
	return 0;
}

static void
tasks_run(ProtobufCGrpc2PollLoop *loop)
{
	while (loop->tasks != NULL) {
		Task *t = loop->tasks;

		loop->tasks = t->next;
		if (loop->tasks == NULL)
			loop->tasks_tail = NULL;
		t->fn(t->arg);
		free(t);
	}
}

/* ---- posting from other threads ---- */

static int
poll_post(ProtobufCGrpc2IoSystem *io, void (*fn)(void *), void *arg)
{
	ProtobufCGrpc2PollLoop *loop = ((PollIo *) io)->loop;
	Task *t = malloc(sizeof(*t));
	char b = 'x';

	if (t == NULL)
		return -1;
	t->next = NULL;
	t->fn = fn;
	t->arg = arg;
	pthread_mutex_lock(&loop->post_mutex);
	if (loop->posted_tail != NULL)
		loop->posted_tail->next = t;
	else
		loop->posted = t;
	loop->posted_tail = t;
	pthread_mutex_unlock(&loop->post_mutex);
	/* a full pipe means a wake-up is pending anyway */
	if (write(loop->wake[1], &b, 1) < 0) {
		/* EAGAIN or EINTR: nothing to do */
	}
	return 0;
}

/* Move what other threads posted to the end of the deferred tasks. */
static void
posted_collect(ProtobufCGrpc2PollLoop *loop)
{
	Task *list, *tail;

	pthread_mutex_lock(&loop->post_mutex);
	list = loop->posted;
	tail = loop->posted_tail;
	loop->posted = loop->posted_tail = NULL;
	pthread_mutex_unlock(&loop->post_mutex);
	if (list == NULL)
		return;
	if (loop->tasks_tail != NULL)
		loop->tasks_tail->next = list;
	else
		loop->tasks = list;
	loop->tasks_tail = tail;
}

static void
wake_drain(ProtobufCGrpc2PollLoop *loop)
{
	char buf[64];

	while (read(loop->wake[0], buf, sizeof buf) > 0)
		;
}

/* ---- entries ---- */

static void
entry_link(ProtobufCGrpc2PollLoop *loop, Entry *e, int kind, int fd)
{
	e->kind = kind;
	e->fd = fd;
	e->events = 0;
	e->closing = 0;
	e->prev = NULL;
	e->next = loop->entries;
	if (loop->entries != NULL)
		loop->entries->prev = e;
	loop->entries = e;
}

/* Take the entry out of the poll set and close its descriptor. The memory
 * stays valid until the task that frees it runs. */
static void
entry_close(ProtobufCGrpc2PollLoop *loop, Entry *e)
{
	if (e->closing)
		return;
	e->closing = 1;
	e->events = 0;
	if (e->prev != NULL)
		e->prev->next = e->next;
	else
		loop->entries = e->next;
	if (e->next != NULL)
		e->next->prev = e->prev;
	e->prev = e->next = NULL;
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
stream_new(ProtobufCGrpc2PollLoop *loop, int fd)
{
	Stream *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return NULL;
	s->loop = loop;
	entry_link(loop, &s->e, K_STREAM, fd);
	loop->n_objects++;
	return s;
}

static void
stream_update_events(Stream *s)
{
	s->e.events = (short) (((s->started && !s->eof_seen) ? POLLIN : 0) |
			       (s->woff < s->wlen ? POLLOUT : 0));
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
poll_stream_start(ProtobufCGrpc2IoSystem *io, void *stream,
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
poll_stream_write(ProtobufCGrpc2IoSystem *io, void *stream,
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
	ProtobufCGrpc2PollLoop *loop = s->loop;
	void (*closed)(void *) = s->handler.closed;
	void *data = s->handler.data;
	int started = s->started;

	free(s->wbuf);
	free(s);
	loop->n_objects--;
	if (started && closed != NULL)
		closed(data);
}

static void
poll_stream_close(ProtobufCGrpc2IoSystem *io, void *stream)
{
	Stream *s = stream;
	(void) io;

	if (s->e.closing)
		return;
	if (s->woff < s->wlen)
		stream_flush(s);	/* best effort */
	entry_close(s->loop, &s->e);
	if (task_post(s->loop, stream_closed_task, s) != 0)
		abort();		/* cannot report completion */
}

static void
stream_dispatch(Stream *s, short revents)
{
	if ((revents & POLLOUT) && s->woff < s->wlen) {
		stream_flush(s);	/* on failure the read side reports it */
		stream_update_events(s);
	}
	if (s->e.closing)
		return;
	if ((revents & (POLLIN | POLLHUP | POLLERR)) && s->started &&
	    !s->eof_seen) {
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
poll_listen(ProtobufCGrpc2IoSystem *io, const char *host, unsigned port,
	    void (*accepted)(void *, void *), void *accepted_data, void **out)
{
	ProtobufCGrpc2PollLoop *loop = ((PollIo *) io)->loop;
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
	l->loop = loop;
	l->accepted = accepted;
	l->accepted_data = accepted_data;
	entry_link(loop, &l->e, K_LISTENER, fd);
	l->e.events = POLLIN;
	loop->n_objects++;
	*out = l;
	return 0;
}

static void
listener_free_task(void *arg)
{
	Listener *l = arg;

	l->loop->n_objects--;
	free(l);
}

static void
poll_unlisten(ProtobufCGrpc2IoSystem *io, void *listener)
{
	Listener *l = listener;
	(void) io;

	if (l->e.closing)
		return;
	entry_close(l->loop, &l->e);
	if (task_post(l->loop, listener_free_task, l) != 0)
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
		    (s = stream_new(l->loop, fd)) == NULL) {
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
	c->loop->n_objects--;
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
	if (task_post(c->loop, connect_report_task, c) != 0)
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
			 * writability, from the loop */
			entry_link(c->loop, &c->e, K_CONNECT, fd);
			c->e.events = POLLOUT;
			return;
		}
		c->last_error = errno;
		close(fd);
	}
	connect_fail(c);
}

static int
poll_connect(ProtobufCGrpc2IoSystem *io, const char *host, unsigned port,
	     void (*connected)(void *, void *, int), void *data, void **handle)
{
	ProtobufCGrpc2PollLoop *loop = ((PollIo *) io)->loop;
	struct addrinfo hints;
	char portstr[16];
	Connect *c;
	int rv;

	if (port > 65535)
		return -1;
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return -1;
	c->loop = loop;
	c->port = port;
	c->connected = connected;
	c->data = data;
	c->e.kind = K_CONNECT;
	c->e.fd = -1;
	c->e.closing = 1;		/* not linked yet */
	snprintf(portstr, sizeof portstr, "%u", port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	rv = getaddrinfo(host, portstr, &hints, &c->res);	/* may block */
	loop->n_objects++;
	c->e.closing = 0;
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
poll_cancel_connect(ProtobufCGrpc2IoSystem *io, void *handle)
{
	Connect *c = handle;
	(void) io;

	if (c->cancelled)
		return;
	c->cancelled = 1;
	if (c->e.fd >= 0) {
		/* in flight: nobody else will report it */
		entry_close(c->loop, &c->e);
		if (task_post(c->loop, connect_free_task, c) != 0)
			abort();
	}
	/* otherwise a report task is already queued; it sees `cancelled` */
}

static void
connect_dispatch(Connect *c, short revents)
{
	int err = 0;
	socklen_t elen = sizeof err;
	void (*cb)(void *, void *, int);
	void *data;
	Stream *s;
	int fd;

	if (!(revents & (POLLOUT | POLLERR | POLLHUP | POLLIN)))
		return;
	if (getsockopt(c->e.fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0)
		err = errno;
	if (err != 0) {
		c->last_error = err;
		entry_close(c->loop, &c->e);
		c->e.closing = 0;	/* may be linked again for the next try */
		c->cur = c->cur->ai_next;
		connect_try_next(c);
		return;
	}
	/* connected: the descriptor becomes a stream */
	fd = c->e.fd;
	c->e.fd = -1;
	entry_close(c->loop, &c->e);
	cb = c->connected;
	data = c->data;
	s = stream_new(c->loop, fd);
	if (s == NULL) {
		close(fd);
		c->last_error = ENOMEM;
		connect_fail(c);
		return;
	}
	connect_free(c);
	cb(data, s, 0);
}

/* ---- timers ---- */

static void *
poll_timer_new(ProtobufCGrpc2IoSystem *io, void (*cb)(void *), void *data)
{
	ProtobufCGrpc2PollLoop *loop = ((PollIo *) io)->loop;
	Timer *t = calloc(1, sizeof(*t));

	if (t == NULL)
		return NULL;
	t->callback = cb;
	t->data = data;
	t->next = loop->timers;
	if (loop->timers != NULL)
		loop->timers->prev = t;
	loop->timers = t;
	loop->n_objects++;
	return t;
}

static void
poll_timer_start(ProtobufCGrpc2IoSystem *io, void *timer, uint64_t ms)
{
	Timer *t = timer;
	(void) io;

	t->deadline = now_ms() + ms;
	t->active = 1;
}

static void
poll_timer_stop(ProtobufCGrpc2IoSystem *io, void *timer)
{
	Timer *t = timer;
	(void) io;

	t->active = 0;
}

static void
poll_timer_free(ProtobufCGrpc2IoSystem *io, void *timer)
{
	ProtobufCGrpc2PollLoop *loop = ((PollIo *) io)->loop;
	Timer *t = timer;

	if (t->prev != NULL)
		t->prev->next = t->next;
	else
		loop->timers = t->next;
	if (t->next != NULL)
		t->next->prev = t->prev;
	loop->n_objects--;
	free(t);
}

/* Run the timers that are due. Nothing is remembered across a callback, so
 * callbacks may start, stop and free any timer. Each timer runs at most
 * once per pass, so a timer that restarts itself with 0 ms cannot starve the
 * rest of the loop. */
static void
timers_run(ProtobufCGrpc2PollLoop *loop)
{
	uint64_t now = now_ms();
	uint64_t pass = ++loop->pass;

	for (;;) {
		Timer *t, *due = NULL;

		for (t = loop->timers; t != NULL; t = t->next)
			if (t->active && t->deadline <= now &&
			    t->fired_pass != pass &&
			    (due == NULL || t->deadline < due->deadline))
				due = t;
		if (due == NULL)
			break;
		due->active = 0;
		due->fired_pass = pass;
		due->callback(due->data);
	}
}

/* ms until the first active timer, or -1 */
static int64_t
timers_next_ms(ProtobufCGrpc2PollLoop *loop)
{
	uint64_t now = now_ms();
	int have = 0;
	uint64_t best = 0;
	Timer *t;

	for (t = loop->timers; t != NULL; t = t->next)
		if (t->active && (!have || t->deadline < best)) {
			best = t->deadline;
			have = 1;
		}
	if (!have)
		return -1;
	return best <= now ? 0 : (int64_t) (best - now);
}

/* ---- hooks ---- */

static void *
poll_add_hook(ProtobufCGrpc2IoSystem *io, void (*hook)(void *), void *data)
{
	ProtobufCGrpc2PollLoop *loop = ((PollIo *) io)->loop;
	Hook *h = calloc(1, sizeof(*h));

	if (h == NULL)
		return NULL;
	h->hook = hook;
	h->data = data;
	h->next = loop->hooks;
	loop->hooks = h;
	loop->n_objects++;
	return h;
}

static void
hooks_sweep(ProtobufCGrpc2PollLoop *loop)
{
	Hook **pp = &loop->hooks;

	while (*pp != NULL) {
		Hook *h = *pp;

		if (h->removed) {
			*pp = h->next;
			loop->n_objects--;
			free(h);
		} else {
			pp = &h->next;
		}
	}
}

static void
poll_remove_hook(ProtobufCGrpc2IoSystem *io, void *hook)
{
	ProtobufCGrpc2PollLoop *loop = ((PollIo *) io)->loop;
	Hook *h = hook;

	h->removed = 1;
	if (!loop->in_hooks)
		hooks_sweep(loop);
}

static void
hooks_run(ProtobufCGrpc2PollLoop *loop)
{
	Hook *h;

	loop->in_hooks = 1;
	/* hooks added by a hook go to the front and run next time */
	for (h = loop->hooks; h != NULL; h = h->next)
		if (!h->removed)
			h->hook(h->data);
	loop->in_hooks = 0;
	hooks_sweep(loop);
}

/* ---- the loop ---- */

static int
loop_alive(ProtobufCGrpc2PollLoop *loop)
{
	Entry *e;
	Timer *t;
	int posted;

	pthread_mutex_lock(&loop->post_mutex);
	posted = loop->posted != NULL;
	pthread_mutex_unlock(&loop->post_mutex);
	if (loop->tasks != NULL || posted)
		return 1;
	for (t = loop->timers; t != NULL; t = t->next)
		if (t->active)
			return 1;
	for (e = loop->entries; e != NULL; e = e->next)
		if (e->events != 0)
			return 1;
	return 0;
}

static void
dispatch(ProtobufCGrpc2PollLoop *loop, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		Entry *e = loop->ents[i];
		short rev = loop->pfds[i].revents;

		if (rev == 0 || e->closing)
			continue;
		if (rev & POLLNVAL) {		/* should not happen */
			e->events = 0;
			continue;
		}
		switch (e->kind) {
		case K_STREAM:
			stream_dispatch((Stream *) e, rev);
			break;
		case K_LISTENER:
			listener_dispatch((Listener *) e);
			break;
		case K_CONNECT:
			connect_dispatch((Connect *) e, rev);
			break;
		}
	}
}

int
protobuf_c_grpc2_poll_loop_run_once(ProtobufCGrpc2PollLoop *loop,
				    int timeout_ms)
{
	size_t n = 0;
	Entry *e;
	int64_t wait;
	int64_t tn;

	posted_collect(loop);
	tasks_run(loop);
	timers_run(loop);
	hooks_run(loop);
	if (!loop_alive(loop))
		return 0;

	for (e = loop->entries; e != NULL; e = e->next)
		n += e->events != 0;
	if (n + 1 > loop->cap) {
		size_t cap = loop->cap ? loop->cap : 16;
		struct pollfd *pf;
		Entry **en;

		while (cap < n + 1)
			cap *= 2;
		pf = realloc(loop->pfds, cap * sizeof(*pf));
		if (pf == NULL)
			abort();
		loop->pfds = pf;
		en = realloc(loop->ents, cap * sizeof(*en));
		if (en == NULL)
			abort();
		loop->ents = en;
		loop->cap = cap;
	}
	n = 0;
	for (e = loop->entries; e != NULL; e = e->next)
		if (e->events != 0) {
			loop->pfds[n].fd = e->fd;
			loop->pfds[n].events = e->events;
			loop->pfds[n].revents = 0;
			loop->ents[n++] = e;
		}

	wait = timeout_ms;
	if (loop->tasks != NULL)
		wait = 0;
	tn = timers_next_ms(loop);
	if (tn >= 0 && (wait < 0 || tn < wait))
		wait = tn;
	if (wait > 0x7fffffff)
		wait = 0x7fffffff;

	/* the wake-up pipe goes last, after the n entries */
	loop->pfds[n].fd = loop->wake[0];
	loop->pfds[n].events = POLLIN;
	loop->pfds[n].revents = 0;
	if (poll(loop->pfds, (nfds_t) n + 1, (int) wait) > 0) {
		if (loop->pfds[n].revents != 0)
			wake_drain(loop);
		dispatch(loop, n);
	}
	posted_collect(loop);
	return loop_alive(loop);
}

void
protobuf_c_grpc2_poll_loop_run(ProtobufCGrpc2PollLoop *loop)
{
	while (!loop->stopped && protobuf_c_grpc2_poll_loop_run_once(loop, -1))
		;
	loop->stopped = 0;
}

void
protobuf_c_grpc2_poll_loop_stop(ProtobufCGrpc2PollLoop *loop)
{
	loop->stopped = 1;
}

ProtobufCGrpc2PollLoop *
protobuf_c_grpc2_poll_loop_new(void)
{
	ProtobufCGrpc2PollLoop *loop = calloc(1, sizeof(*loop));
	int i;

	if (loop == NULL)
		return NULL;
	if (pipe(loop->wake) != 0) {
		free(loop);
		return NULL;
	}
	for (i = 0; i < 2; i++)
		if (set_nonblock(loop->wake[i]) != 0)
			break;
	if (i < 2 || pthread_mutex_init(&loop->post_mutex, NULL) != 0) {
		close(loop->wake[0]);
		close(loop->wake[1]);
		free(loop);
		return NULL;
	}
	return loop;
}

int
protobuf_c_grpc2_poll_loop_close(ProtobufCGrpc2PollLoop *loop)
{
	if (loop->n_objects != 0 || loop->tasks != NULL)
		return -1;
	free(loop->pfds);
	free(loop->ents);
	while (loop->posted != NULL) {		/* never run: not delivered */
		Task *t = loop->posted;

		loop->posted = t->next;
		free(t);
	}
	close(loop->wake[0]);
	close(loop->wake[1]);
	pthread_mutex_destroy(&loop->post_mutex);
	free(loop);
	return 0;
}

/* ---- the I/O system ---- */

/* Like g_object_ref_sink(): the first reference taken claims the creation
 * reference instead of adding one. */
static void
poll_ref(ProtobufCGrpc2IoSystem *io)
{
	PollIo *p = (PollIo *) io;

	if (p->floating)
		p->floating = 0;
	else
		p->refs++;
}

static void
poll_destroy(ProtobufCGrpc2IoSystem *io)
{
	PollIo *p = (PollIo *) io;

	if (--p->refs > 0)
		return;
	p->loop->n_objects--;
	free(p);
}

ProtobufCGrpc2IoSystem *
protobuf_c_grpc2_io_system_new_poll(ProtobufCGrpc2PollLoop *loop)
{
	PollIo *io = calloc(1, sizeof(*io));

	if (io == NULL)
		return NULL;
	io->loop = loop;
	io->base.listen = poll_listen;
	io->base.unlisten = poll_unlisten;
	io->base.connect = poll_connect;
	io->base.cancel_connect = poll_cancel_connect;
	io->base.stream_start = poll_stream_start;
	io->base.stream_write = poll_stream_write;
	io->base.stream_close = poll_stream_close;
	io->base.timer_new = poll_timer_new;
	io->base.timer_start = poll_timer_start;
	io->base.timer_stop = poll_timer_stop;
	io->base.timer_free = poll_timer_free;
	io->base.add_iteration_hook = poll_add_hook;
	io->base.remove_iteration_hook = poll_remove_hook;
	io->base.destroy = poll_destroy;
	io->base.ref = poll_ref;
	io->base.post = poll_post;
	io->refs = 1;
	io->floating = 1;
	loop->n_objects++;
	return &io->base;
}
