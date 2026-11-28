/*
 * poll(2) adapter for protobuf-c-grpc2 (library: protobuf-c-grpc2-poll).
 *
 * A small single-threaded event loop with no dependencies beyond POSIX
 * sockets, for programs that have no event loop of their own. The loop is an
 * object separate from the I/O systems that run on it, so a server and a
 * client (each of which owns an I/O system) can share one loop.
 *
 * The loop is single-threaded, except that the I/O system's `post` function
 * (used by thread safe clients) may be called from other threads; it wakes the
 * loop through a pipe that the loop owns.
 *
 * Limitations: host names given to `connect` are resolved with a blocking
 * getaddrinfo() call (numeric addresses never block); `listen` takes address
 * literals only, like the other adapters; POSIX only.
 */
#ifndef PROTOBUF_C_GRPC2_POLL_H
#define PROTOBUF_C_GRPC2_POLL_H

#include "protobuf-c-grpc2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ProtobufCGrpc2PollLoop ProtobufCGrpc2PollLoop;

/** Create an event loop. Returns NULL on failure. */
ProtobufCGrpc2PollLoop *protobuf_c_grpc2_poll_loop_new(void);

/**
 * Options for protobuf_c_grpc2_io_system_new_poll(). Like the other options
 * structs of this library, set `struct_size` to sizeof the struct (the _INIT
 * macro does) and leave the fields you do not use zeroed.
 *
 *   ProtobufCGrpc2PollOptions o = PROTOBUF_C_GRPC2_POLL_OPTIONS_INIT;
 *   o.loop = loop;
 *   io = protobuf_c_grpc2_io_system_new_poll(&o);
 */
typedef struct ProtobufCGrpc2PollOptions {
	/** sizeof(ProtobufCGrpc2PollOptions). */
	size_t struct_size;
	/** Required: the loop to run on. */
	ProtobufCGrpc2PollLoop *loop;
} ProtobufCGrpc2PollOptions;

#define PROTOBUF_C_GRPC2_POLL_OPTIONS_INIT { sizeof(ProtobufCGrpc2PollOptions) }

/**
 * An I/O system that runs on `options->loop`. It is created floating: the first
 * server or client to take it claims the creation reference, and further ones
 * share it. If you use it without one, call `ref` and later `destroy`.
 * The loop must outlive it.
 */
ProtobufCGrpc2IoSystem *
protobuf_c_grpc2_io_system_new_poll(const ProtobufCGrpc2PollOptions *options);

/**
 * Run the loop until nothing is left to wait for (no active connection,
 * listener or timer) or protobuf_c_grpc2_poll_loop_stop() is called.
 */
void protobuf_c_grpc2_poll_loop_run(ProtobufCGrpc2PollLoop *loop);

/**
 * Run one iteration: dispatch whatever is due and ready, waiting at most
 * `timeout_ms` milliseconds for something to happen (0: do not wait, -1:
 * wait as long as it takes). Returns nonzero if the loop still has something
 * to wait for. Useful to drive the loop from another one.
 */
int protobuf_c_grpc2_poll_loop_run_once(ProtobufCGrpc2PollLoop *loop,
					int timeout_ms);

/** Make a running protobuf_c_grpc2_poll_loop_run() return. */
void protobuf_c_grpc2_poll_loop_stop(ProtobufCGrpc2PollLoop *loop);

/**
 * Free the loop. Returns 0, or -1 (and does nothing) if I/O systems,
 * streams, listeners or timers created on it are still alive, as for
 * uv_loop_close().
 */
int protobuf_c_grpc2_poll_loop_close(ProtobufCGrpc2PollLoop *loop);

#ifdef __cplusplus
}
#endif

#endif /* PROTOBUF_C_GRPC2_POLL_H */
