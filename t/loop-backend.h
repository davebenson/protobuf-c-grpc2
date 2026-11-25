/*
 * The event loop under test. The endpoint and client tests run unchanged on
 * every I/O backend: each test program is linked with exactly one of
 * loop-backend-uv.c and loop-backend-poll.c. They use only the
 * ProtobufCGrpc2IoSystem interface (including its timers) from here on.
 */
#ifndef LOOP_BACKEND_H
#define LOOP_BACKEND_H

#include "protobuf-c-grpc2/protobuf-c-grpc2.h"

typedef struct TestLoop TestLoop;

const char *test_backend_name(void);
TestLoop *test_loop_new(void);
/* A new I/O system on the loop (owned by whatever it is given to). */
ProtobufCGrpc2IoSystem *test_loop_new_io(TestLoop *loop);
/* Run until nothing is left to wait for. */
void test_loop_run(TestLoop *loop);
/* Free the loop. Returns 0, or nonzero if something was leaked on it. */
int test_loop_close(TestLoop *loop);

#endif
