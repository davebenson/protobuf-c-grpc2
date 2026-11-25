# protobuf-c-grpc2

gRPC over HTTP/2 for protobuf-c, built on [nghttp2 version 2][ng2]
(`libnghttp2v2`).

Servers and clients are created from an options struct that names pluggable
event-loop and TLS components, so the core has no dependency on any particular event
loop or TLS library. Unary calls and all three streaming modes are
supported.

## Requirements

* **protobuf-c with streaming support** (the `ProtobufCService::invoke`
  streaming extensions and `protoc-gen-c` method flags). This is not yet in
  a protobuf-c release; it is on the `streaming-support` branch of
  <https://github.com/davebenson/protobuf-c>.
* **nghttp2 version 2** (`libnghttp2v2`, pkg-config `libnghttp2v2`).
* Optionally libuv and OpenSSL, for the adapters below.

The libraries build against *installed* copies of these, so this package can
be released and packaged separately from protobuf-c itself.

It builds four libraries, each with its own pkg-config file and headers, so
distributions can split it into separate packages:

| library                     | needs            | provides |
|-----------------------------|------------------|----------|
| `protobuf-c-grpc2`          | protobuf-c, nghttp2-v2 | the HTTP/2+gRPC engine, servers and clients, and the plugin interfaces (no event loop or TLS) |
| `protobuf-c-grpc2-uv`       | libuv            | a libuv I/O system |
| `protobuf-c-grpc2-poll`     | POSIX            | a small poll(2) event loop and I/O system, for programs without an event loop of their own |
| `protobuf-c-grpc2-openssl`  | OpenSSL          | an OpenSSL secure transport |

`--without-uv` / `--without-poll` / `--without-openssl` (or `-DWITH_UV=OFF` /
`-DWITH_POLL=OFF` / `-DWITH_OPENSSL=OFF`) skip an adapter; they are otherwise built when found.

## Building

    autoreconf -i && ./configure && make && make check
    # or
    cmake -S . -B build && cmake --build build && ctest --test-dir build

Tests and examples need `protoc` and an installed `protoc-gen-c`
(`--with-protoc`, `--with-protoc-gen-c` select them).

## Options structs

Servers and clients are configured with one options struct each, passed to
one constructor. Initialise it with the `*_INIT` macro, which sets
`struct_size` (it must be the first thing set: the library reads only that
many bytes, so a program built against an older version of a struct keeps
working when fields are added), then set what you need and leave the rest
zero.

## Building a server

    ProtobufCService *services[] = { &my_service.base };
    ProtobufCGrpc2ServerOptions o = PROTOBUF_C_GRPC2_SERVER_OPTIONS_INIT;
    o.services = services;
    o.n_services = 1;
    o.io = protobuf_c_grpc2_io_system_new_uv(loop);                       /* -uv */
    o.secure_transport =                                          /* optional TLS */
        protobuf_c_grpc2_secure_transport_system_new_openssl(ssl_ctx);   /* -openssl */
    ProtobufCGrpc2Server *server = protobuf_c_grpc2_server_new(&o);
    protobuf_c_grpc2_server_listen(server, "0.0.0.0", 50051);
    /* ... run the loop ...; then: */
    protobuf_c_grpc2_server_free(server);

Without `secure_transport` connections are cleartext HTTP/2 (h2c), which is
what you want behind a TLS-terminating load balancer. With it, TLS is
required and ALPN must negotiate `h2`. On success the server takes its own
references to `io` and `secure_transport` (so either can be shared with other
servers and clients); if `protobuf_c_grpc2_server_new()` fails it takes
nothing.

`io` and `secure_transport` are the interfaces in `protobuf-c-grpc2-io.h`: a
`ProtobufCGrpc2IoSystem` (TCP listen, connect and streams, timers, and a hook
that runs before the loop blocks) and a `ProtobufCGrpc2SecureTransportSystem`
(a sans-I/O, BIO-style TLS session). The adapter libraries provide libuv,
poll(2) and OpenSSL versions; implement the interfaces to support another event loop or
TLS library. The core never sees libuv or OpenSSL.

### Without an event loop: the poll adapter

`protobuf-c-grpc2-poll.h` provides a single-threaded loop that you run
yourself. The loop is separate from the I/O systems on it, so a server and a
client can share one:

    ProtobufCGrpc2PollLoop *loop = protobuf_c_grpc2_poll_loop_new();
    o.io = protobuf_c_grpc2_io_system_new_poll(loop);
    ...
    protobuf_c_grpc2_poll_loop_run(loop);   /* until idle or _stop(loop) */

`protobuf_c_grpc2_poll_loop_run_once(loop, timeout_ms)` runs one iteration, to
drive the loop from another one, and `protobuf_c_grpc2_poll_loop_close()` frees
it (failing if anything created on it is still alive). Host names given to
`connect` are resolved with a blocking `getaddrinfo()`; numeric addresses
never block. `poll(2)` is linear in the number of descriptors, which is fine
for thousands of connections but not for far more.

The tests under `t/` that need an event loop (`endpoints`, `client`, `bulk`)
run once on each backend, through the small shim in `t/loop-backend.h`.

## Building a client

A client is a "channel" to one server: it connects lazily on the first call
(queueing calls until the connection is up), reconnects after a lost
connection, and backs off after failed attempts.

    ProtobufCGrpc2ClientOptions o = PROTOBUF_C_GRPC2_CLIENT_OPTIONS_INIT;
    o.host = "example.com";
    o.port = 50051;
    o.io = protobuf_c_grpc2_io_system_new_uv(loop);
    o.secure_transport =                                          /* optional TLS */
        protobuf_c_grpc2_secure_transport_system_new_openssl(ssl_ctx);
    ProtobufCGrpc2Client *client = protobuf_c_grpc2_client_new(&o);
    ProtobufCService *svc = protobuf_c_grpc2_client_get_service(client, &desc);
    /* call through svc with the generated callers */
    protobuf_c_grpc2_client_free(client);

For TLS, configure the `SSL_CTX` with the trust roots and
`SSL_VERIFY_PEER`; the server name checked (and sent as SNI) is `host` unless
`server_name` says otherwise. The service handle stays valid across
reconnects.

Connection rules: a lost connection fails the calls in flight with
`UNAVAILABLE`, and the next call reconnects at once. A connection attempt
that fails (or a connection that never gets an answer from the server, such
as a failed TLS handshake) starts a backoff, 1 s growing x1.6 up to 120 s
(`initial_backoff_ms`, `max_backoff_ms`): calls made during it fail
immediately with `UNAVAILABLE` rather than waiting. A connection attempt
gives up after 20 s (`connect_timeout_ms`). There is one connection per
client and no name-resolution or load-balancing policy beyond trying the
host's addresses in order.

## Timeouts

A client sets a timeout per service handle (handles are cheap, so use one
per timeout):

    ProtobufCService *svc = protobuf_c_grpc2_conn_client_service(conn, &desc);
    protobuf_c_grpc2_client_service_set_timeout_ms(svc, 2000);

Calls made through it get a deadline, sent as `grpc-timeout`. A call past its
deadline is cancelled and completes with `PROTOBUF_C_GRPC2_DEADLINE_EXCEEDED`
(see `on_call_done`). A server enforces the client's `grpc-timeout` too
(`ProtobufCGrpc2ServerOptions` has `default_timeout_ms` for calls without one
and `max_timeout_ms` as a cap); a malformed `grpc-timeout` is ignored. The
handler is not yet told that its call timed out: it can keep running and its
later responses are dropped.

Deadlines are driven by the same timer as the HTTP/2 protocol timers
(`protobuf_c_grpc2_conn_get_timeout_ns()` / `..._handle_expiry()`); the
built-in pump does this for you.

## Without the built-in pump (sans-I/O)

You can also own the sockets yourself:

* Server: `protobuf_c_grpc2_server_new(NULL)` and `..._add_service()`, then
  `protobuf_c_grpc2_conn_server_new()` per accepted connection.
* Client: `protobuf_c_grpc2_conn_client_new()` (options:
  `ProtobufCGrpc2ConnOptions`, initialised with
  `PROTOBUF_C_GRPC2_CONN_OPTIONS_INIT`), then
  `protobuf_c_grpc2_conn_client_service()` for a `ProtobufCService` of a
  generated service descriptor, and call the generated caller functions.
* Move bytes with `protobuf_c_grpc2_conn_read()` and
  `protobuf_c_grpc2_conn_write()`.

Streaming methods use `ProtobufCService::invoke` as documented in
`protobuf-c.h`: `PROTOBUF_C_OUTPUT_STREAMING` calls deliver many responses and
a final NULL; `PROTOBUF_C_INPUT_STREAMING` calls return a
typed stream, `<Request>__InputStream`, on which the caller sends requests with
the generated `<request>__input_stream__send()` and ends the stream with
`__end()` and `__destroy()` (or the generic `protobuf_c_input_stream_*`
functions on its `base` member, which check the message type at run time).
Stream implementations set `input_descriptor` to the request type. Errors are reported through the
`on_call_done` option.

## Examples

`examples/` has a trivial libuv server and client for a small Greeter service
(`greeter.proto`: one unary, one server-streaming and one client-streaming
method), both written with the options structs:

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

## License

BSD 2-clause, the same as protobuf-c; see `LICENSE`.
