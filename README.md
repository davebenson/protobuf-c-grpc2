# protobuf-c-grpc2

gRPC over HTTP/2 for protobuf-c, built on [nghttp2 version 2][ng2]
(`libnghttp2v2`).

This directory is a **separate package**. It has its own `configure.ac` and
`CMakeLists.txt` and builds against an *installed* `libprotobuf-c` and
`libnghttp2v2`, so protobuf-c itself has no dependency on nghttp2.

It builds three libraries, each with its own pkg-config file and headers, so
distributions can split it into separate packages:

| library                     | needs            | provides |
|-----------------------------|------------------|----------|
| `protobuf-c-grpc2`          | protobuf-c, nghttp2-v2 | the HTTP/2+gRPC engine, the server builder, and the plugin interfaces (no event loop or TLS) |
| `protobuf-c-grpc2-uv`       | libuv            | a libuv I/O system |
| `protobuf-c-grpc2-openssl`  | OpenSSL          | an OpenSSL secure transport |

`--without-uv` / `--without-openssl` (or `-DWITH_UV=OFF` /
`-DWITH_OPENSSL=OFF`) skip an adapter; they are otherwise built when found.

## Building

    cd grpc2
    autoreconf -i && ./configure && make && make check
    # or
    cmake -S . -B build && cmake --build build && ctest --test-dir build

Tests and examples need `protoc` and an installed `protoc-gen-c`
(`--with-protoc`, `--with-protoc-gen-c` select them).

## Building a server

The builder assembles a server from services and pluggable components:

    ProtobufCGrpc2ServerBuilder *b = protobuf_c_grpc2_server_builder_new();
    protobuf_c_grpc2_server_builder_add_service(b, &my_service.base);
    protobuf_c_grpc2_server_builder_use_uv(b, loop);          /* -uv library */
    protobuf_c_grpc2_server_builder_use_openssl(b, ssl_ctx);  /* optional TLS */
    ProtobufCGrpc2Server *server = protobuf_c_grpc2_server_from_builder(b);
    protobuf_c_grpc2_server_builder_free(b);
    protobuf_c_grpc2_server_listen(server, "0.0.0.0", 50051);
    /* ... run the loop ...; then: */
    protobuf_c_grpc2_server_free(server);

Without `use_openssl` connections are cleartext HTTP/2 (h2c), which is what
you want behind a TLS-terminating load balancer. With it, TLS is required and
ALPN must negotiate `h2`.

`use_uv` and `use_openssl` are shorthands for the generic
`add_io_system()` and `add_secure_transport_system()`, which take the
interfaces in `protobuf-c-grpc2-io.h`: a `ProtobufCGrpc2IoSystem` (TCP
listen/streams, timers and a hook that runs before the loop blocks) and a
`ProtobufCGrpc2SecureTransportSystem` (a sans-I/O, BIO-style TLS session).
Implement those to support another event loop or TLS library; the core never
sees libuv or OpenSSL.

## Building a client

The client builder is the mirror image. A client is a "channel" to one
server: it connects lazily on the first call (queueing calls until the
connection is up), reconnects after a lost connection, and backs off after
failed attempts.

    ProtobufCGrpc2ClientBuilder *b = protobuf_c_grpc2_client_builder_new();
    protobuf_c_grpc2_client_builder_set_target(b, "example.com", 50051);
    protobuf_c_grpc2_client_builder_use_uv(b, loop);          /* -uv library */
    protobuf_c_grpc2_client_builder_use_openssl(b, ssl_ctx);  /* optional TLS */
    ProtobufCGrpc2Client *client = protobuf_c_grpc2_client_from_builder(b);
    protobuf_c_grpc2_client_builder_free(b);
    ProtobufCService *svc = protobuf_c_grpc2_client_get_service(client, &desc);
    /* call through svc with the generated callers */
    protobuf_c_grpc2_client_free(client);

For TLS, configure the `SSL_CTX` with the trust roots and
`SSL_VERIFY_PEER`; the server name checked (and sent as SNI) is the target
host unless `..._set_server_name()` says otherwise. The service handle stays
valid across reconnects.

Connection rules: a lost connection fails the calls in flight with
`UNAVAILABLE`, and the next call reconnects at once. A connection attempt
that fails (or a connection that never gets an answer from the server, such
as a failed TLS handshake) starts a backoff, 1 s growing x1.6 up to 120 s
(`..._set_backoff()`): calls made during it fail immediately with
`UNAVAILABLE` rather than waiting. A connection attempt gives up after 20 s
(`..._set_connect_timeout_ms()`). There is one connection per client and no
name-resolution or load-balancing policy beyond trying the host's addresses
in order.

## Timeouts

A client sets a timeout per service handle (handles are cheap, so use one
per timeout):

    ProtobufCService *svc = protobuf_c_grpc2_conn_client_service(conn, &desc);
    protobuf_c_grpc2_client_service_set_timeout_ms(svc, 2000);

Calls made through it get a deadline, sent as `grpc-timeout`. A call past its
deadline is cancelled and completes with `PROTOBUF_C_GRPC2_DEADLINE_EXCEEDED`
(see `on_call_done`). A server enforces the client's `grpc-timeout` too
(`ProtobufCGrpc2ConnOptions` has `default_timeout_ms` for calls without one
and `max_timeout_ms` as a cap); a malformed `grpc-timeout` is ignored. The
handler is not yet told that its call timed out: it can keep running and its
later responses are dropped.

Deadlines are driven by the same timer as the HTTP/2 protocol timers
(`protobuf_c_grpc2_conn_get_timeout_ns()` / `..._handle_expiry()`); the
builder's pump does this for you.

## Without the builder (sans-I/O)

You can also own the sockets yourself:

* Server: `protobuf_c_grpc2_server_new()` and `..._add_service()`, then
  `protobuf_c_grpc2_conn_server_new()` per accepted connection.
* Client: `protobuf_c_grpc2_conn_client_new()`, then
  `protobuf_c_grpc2_conn_client_service()` for a `ProtobufCService` of a
  generated service descriptor, and call the generated caller functions.
* Move bytes with `protobuf_c_grpc2_conn_read()` and
  `protobuf_c_grpc2_conn_write()`.

Streaming methods use `ProtobufCService::invoke` as documented in
`protobuf-c.h`: `PROTOBUF_C_OUTPUT_STREAMING` calls deliver many responses and
a final NULL; `PROTOBUF_C_INPUT_STREAMING` calls return a
`ProtobufCInputStream` on which the caller sends requests and ends the
stream with `send_input(NULL)`. Errors are reported through
`ProtobufCGrpc2ConnOptions::on_call_done`.

## Examples

`examples/` has a trivial libuv server and client for a small Greeter service
(`greeter.proto`: one unary, one server-streaming and one client-streaming
method), both written with the builders:

    ./examples/libuv-server 50051 &
    ./examples/libuv-client 127.0.0.1 50051 world

Pass a certificate and key to the server (`libuv-server PORT cert.pem
key.pem`) to serve TLS, and a CA file to the client (`libuv-client HOST PORT
NAME ca.pem`) to verify it. They are built when libuv is found
(`--disable-examples` / `-DBUILD_EXAMPLES=OFF` to skip), and `make check` /
`ctest` runs them against each other, over TLS too when the `openssl` tool
is available.

Limitations: no message compression, no metadata API, and no cancellation
or deadline notification for handlers. The client has no retries, load
balancing or name-resolution policies, and uses a single connection. Nothing
is thread safe.

[ng2]: https://github.com/nghttp2/nghttp2
