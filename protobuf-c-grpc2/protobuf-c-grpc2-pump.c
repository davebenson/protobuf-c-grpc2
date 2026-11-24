/*
 * The connection pump: moves bytes between a pluggable I/O stream, an
 * optional pluggable TLS session and a ProtobufCGrpc2Conn. The same code
 * serves accepted (server) and outgoing (client) connections.
 */
#include <stdlib.h>
#include <string.h>

#include "protobuf-c-grpc2-internal.h"

#define IO_CHUNK 16384

static void
pump_timer_cb(void *data)
{
	ConnPump *p = data;
	if (!p->closing && protobuf_c_grpc2_conn_handle_expiry(p->conn) < 0)
		protobuf_c_grpc2__pump_close(p);
}

/* 1 when application data may flow; closes the connection on failure. */
static int
pump_tls_ready(ConnPump *p)
{
	int st = p->tls->vtable->handshake_state(p->tls);

	if (st < 0) {
		protobuf_c_grpc2__pump_close(p);
		return 0;
	}
	if (st == 0)
		return 0;
	if (!p->alpn_checked) {
		const char *alpn = p->tls->vtable->alpn(p->tls);
		if (alpn == NULL || strcmp(alpn, "h2") != 0) {
			protobuf_c_grpc2__pump_close(p);	/* gRPC needs HTTP/2 */
			return 0;
		}
		p->alpn_checked = 1;
	}
	return 1;
}

static int
pump_conn_read(ConnPump *p, const uint8_t *bytes, size_t len)
{
	if (len > 0)
		p->established = 1;
	return protobuf_c_grpc2_conn_read(p->conn, bytes, len);
}

static void
pump_received(void *data, const uint8_t *bytes, size_t len)
{
	ConnPump *p = data;
	uint8_t buf[IO_CHUNK];

	if (p->closing)
		return;
	if (p->tls == NULL) {
		if (pump_conn_read(p, bytes, len) < 0)
			protobuf_c_grpc2__pump_close(p);
		return;
	}
	if (p->tls->vtable->feed_ciphertext(p->tls, bytes, len) < 0) {
		protobuf_c_grpc2__pump_close(p);
		return;
	}
	if (!pump_tls_ready(p))
		return;
	for (;;) {
		ssize_t n = p->tls->vtable->read_plaintext(p->tls, buf, sizeof buf);
		if (n < 0) {
			protobuf_c_grpc2__pump_close(p);
			return;
		}
		if (n == 0)
			return;
		if (pump_conn_read(p, buf, (size_t) n) < 0 || p->closing) {
			protobuf_c_grpc2__pump_close(p);
			return;
		}
	}
}

static void
pump_eof(void *data, int error)
{
	(void) error;
	protobuf_c_grpc2__pump_close(data);
}

void
protobuf_c_grpc2__pump_flush(ConnPump *p)
{
	ProtobufCGrpc2IoSystem *io = p->io;
	uint8_t buf[IO_CHUNK];
	ssize_t n;
	int64_t t;

	if (p->closing)
		return;
	if (p->tls == NULL) {
		while ((n = protobuf_c_grpc2_conn_write(p->conn, buf, sizeof buf)) > 0)
			if (io->stream_write(io, p->stream, buf, (size_t) n) != 0) {
				protobuf_c_grpc2__pump_close(p);
				return;
			}
		if (n < 0) {
			protobuf_c_grpc2__pump_close(p);
			return;
		}
	} else {
		if (pump_tls_ready(p)) {
			while ((n = protobuf_c_grpc2_conn_write(p->conn, buf, sizeof buf)) > 0)
				if (p->tls->vtable->write_plaintext(p->tls, buf,
								    (size_t) n) < 0) {
					protobuf_c_grpc2__pump_close(p);
					return;
				}
			if (n < 0) {
				protobuf_c_grpc2__pump_close(p);
				return;
			}
		}
		if (p->closing)
			return;
		while ((n = p->tls->vtable->read_ciphertext(p->tls, buf,
							    sizeof buf)) > 0)
			if (io->stream_write(io, p->stream, buf, (size_t) n) != 0) {
				protobuf_c_grpc2__pump_close(p);
				return;
			}
	}
	t = protobuf_c_grpc2_conn_get_timeout_ns(p->conn);
	if (t < 0)
		io->timer_stop(io, p->timer);
	else
		io->timer_start(io, p->timer, (uint64_t) t / 1000000u + 1);
}

static void
pump_closed(void *data)
{
	ConnPump *p = data;
	void (*on_closed)(void *, int) = p->on_closed;
	void *arg = p->arg;
	int established = p->established;

	p->io->timer_free(p->io, p->timer);
	if (p->tls != NULL)
		p->tls->vtable->destroy(p->tls);
	if (p->on_closing != NULL)
		p->on_closing(p, arg);
	protobuf_c_grpc2_conn_free(p->conn);	/* completes unfinished calls */
	free(p);
	if (on_closed != NULL)
		on_closed(arg, established);
}

void
protobuf_c_grpc2__pump_close(ConnPump *p)
{
	if (p->closing)
		return;
	p->closing = 1;
	p->io->stream_close(p->io, p->stream);
}

ConnPump *
protobuf_c_grpc2__pump_new(ProtobufCGrpc2IoSystem *io, void *stream,
			   ProtobufCGrpc2Conn *conn,
			   ProtobufCGrpc2SecureTransport *tls,
			   void (*on_closing)(ConnPump *, void *),
			   void (*on_closed)(void *, int), void *arg)
{
	ConnPump *p = calloc(1, sizeof(*p));
	ProtobufCGrpc2IoHandler h;

	if (p == NULL) {
		io->stream_close(io, stream);
		return NULL;
	}
	p->io = io;
	p->stream = stream;
	p->conn = conn;
	p->tls = tls;
	p->on_closing = on_closing;
	p->on_closed = on_closed;
	p->arg = arg;
	p->timer = io->timer_new(io, pump_timer_cb, p);
	if (p->timer == NULL) {
		free(p);
		io->stream_close(io, stream);
		return NULL;
	}
	h.received = pump_received;
	h.eof = pump_eof;
	h.closed = pump_closed;
	h.data = p;
	if (io->stream_start(io, stream, &h) != 0) {
		/* the stream was never started: closing it frees it quietly */
		io->timer_free(io, p->timer);
		free(p);
		io->stream_close(io, stream);
		return NULL;
	}
	return p;
}
