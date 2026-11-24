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
listen/streams, timers and an end-of-loop-iteration hook) and a
`ProtobufCGrpc2SecureTransportSystem` (a sans-I/O, BIO-style TLS session).
Implement those to support another event loop or TLS library; the core never
sees libuv or OpenSSL.

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
method):

    ./examples/libuv-server 50051 &
    ./examples/libuv-client 127.0.0.1 50051 world

The server uses the builder (`libuv-server PORT [CERT.pem KEY.pem]` serves TLS
when given a certificate). The client does not have a builder yet, so it uses
`examples/uv-grpc2.h`, a small hand-written libuv integration of the sans-I/O
API (about 200 lines): feed socket reads to `protobuf_c_grpc2_conn_read()`,
flush `protobuf_c_grpc2_conn_write()` once per loop iteration, and map
`protobuf_c_grpc2_conn_get_timeout_ns()` to a timer. They are built
when libuv is found (`--disable-examples` / `-DBUILD_EXAMPLES=OFF` to skip)
and `make check` / `ctest` runs them against each other.

Limitations: no client builder yet (so no TLS in the example client; the
OpenSSL adapter already supports client sessions and `t/builder.c` uses them),
no message compression, no deadlines (`grpc-timeout`) or metadata API, and no
thread safety.

[ng2]: https://github.com/nghttp2/nghttp2
