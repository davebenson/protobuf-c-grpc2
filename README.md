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
* Optionally libuv, OpenSSL or GnuTLS, for the adapters below.

The libraries build against *installed* copies of these, so this package can
be released and packaged separately from protobuf-c itself.

It builds up to seven libraries, each with its own pkg-config file and headers, so
distributions can split it into separate packages:

| library                     | needs            | provides |
|-----------------------------|------------------|----------|
| `protobuf-c-grpc2`          | protobuf-c, nghttp2-v2 | the HTTP/2+gRPC engine, servers and clients, and the plugin interfaces (no event loop or TLS) |
| `protobuf-c-grpc2-uv`       | libuv            | a libuv I/O system |
| `protobuf-c-grpc2-poll`     | POSIX            | a small poll(2) event loop and I/O system, for programs without an event loop of their own |
| `protobuf-c-grpc2-dispatch` | protobuf-c-rpc (with prepare functions) | an I/O system on protobuf-c-rpc's `ProtobufCRPCDispatch`, to share a main-loop with protobuf-c-rpc services |
| `protobuf-c-grpc2-glib`     | GLib >= 2.36     | an I/O system on a `GMainContext`, to run gRPC from a GLib or GTK main-loop |
| `protobuf-c-grpc2-openssl`  | OpenSSL          | an OpenSSL secure transport |
| `protobuf-c-grpc2-gnutls`   | GnuTLS           | a GnuTLS secure transport |

`--without-uv` / `--without-poll` / `--without-dispatch` / `--without-glib` /
`--without-openssl` / `--without-gnutls` (or `-DWITH_UV=OFF` / `-DWITH_POLL=OFF` /
`-DWITH_DISPATCH=OFF` / `-DWITH_GLIB=OFF` / `-DWITH_OPENSSL=OFF` / `-DWITH_GNUTLS=OFF`) skip an adapter; they are otherwise built when found.

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
poll(2), protobuf-c-rpc dispatch and OpenSSL and GnuTLS versions; implement the interfaces to support another event loop or
TLS library. The core never sees libuv, OpenSSL or GnuTLS.

### TLS with OpenSSL

`protobuf-c-grpc2-openssl.h` makes a secure transport system from an
`SSL_CTX`. It works with any I/O system (it uses memory BIOs, never sockets):

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());          /* a server */
    SSL_CTX_use_certificate_chain_file(ctx, "cert.pem");
    SSL_CTX_use_PrivateKey_file(ctx, "key.pem", SSL_FILETYPE_PEM);
    o.secure_transport = protobuf_c_grpc2_secure_transport_system_new_openssl(ctx);
    SSL_CTX_free(ctx);                                        /* it keeps its own reference */

    SSL_CTX *cctx = SSL_CTX_new(TLS_client_method());         /* a client */
    SSL_CTX_set_default_verify_paths(cctx);                   /* or load_verify_locations */
    SSL_CTX_set_verify(cctx, SSL_VERIFY_PEER, NULL);
    co.secure_transport = protobuf_c_grpc2_secure_transport_system_new_openssl(cctx);
    SSL_CTX_free(cctx);

Configure certificates, keys, trust roots, verification and the protocol
versions on the context as usual. The system takes its own reference to the
context, so you may free yours; it modifies the context in two ways: it
installs an ALPN callback that accepts `h2` (replacing any you set), and it
raises the minimum protocol version to TLS 1.2 if it was lower, as gRPC
requires. Client sessions offer only `h2`, and verify the server (against the
client's `server_name`, by default its `host`) when the context has
`SSL_VERIFY_PEER` set. The system is created floating: the first server or
client to take it claims the creation reference, and further ones share it.

### TLS with GnuTLS

`protobuf-c-grpc2-gnutls.h` is the same thing for GnuTLS. Either adapter
works with any I/O system, and a server and a client in one program may use
different ones:

    gnutls_certificate_credentials_t cred;
    gnutls_certificate_allocate_credentials(&cred);
    gnutls_certificate_set_x509_key_file(cred, "cert.pem", "key.pem",
                                         GNUTLS_X509_FMT_PEM);   /* a server */
    gnutls_certificate_set_x509_system_trust(cred);              /* a client */
    ProtobufCGrpc2GnutlsOptions to = PROTOBUF_C_GRPC2_GNUTLS_OPTIONS_INIT;
    to.credentials = cred;
    o.secure_transport = protobuf_c_grpc2_secure_transport_system_new_gnutls(&to);

Two differences from the OpenSSL adapter. GnuTLS credentials are not
reference counted, so the system takes *ownership* of `credentials` (and frees
them with its last reference, also if creating the system fails); make
credentials for each system. And a client always verifies the server's
certificate chain and name, so it needs trust roots in its credentials; there
is no way to switch verification off. Like the other options structs it has a
`struct_size`, so options can be added later; today the only other one is
`policy`, a GnuTLS priority string (protocol versions, ciphers), which most
programs leave NULL for "NORMAL" limited to TLS 1.2 and later, as gRPC
requires.

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

### With protobuf-c-rpc: the dispatch adapter

A program that already runs `protobuf-c-rpc` services has a
`ProtobufCRPCDispatch`; `protobuf-c-grpc2-dispatch.h` puts gRPC servers and
clients on the same one, so a single main-loop serves both:

    ProtobufCRPCDispatch *d = protobuf_c_rpc_dispatch_default();
    o.io = protobuf_c_grpc2_io_system_new_dispatch(d);   /* NULL: the default */
    ...
    for (;;)
      protobuf_c_rpc_dispatch_run(d);

The dispatch is not owned by the I/O system and must outlive it. It needs a
protobuf-c-rpc with *prepare functions* (`protobuf_c_rpc_dispatch_add_prepare()`),
which is where the core sends the output that callbacks have produced, right
before the dispatch waits. A program that drives the dispatch from its own
loop through `protobuf_c_rpc_dispatch_dispatch()` gets them at the end of each
dispatch, and should call `protobuf_c_rpc_dispatch_run_prepares()` once before
its first wait. Like the poll adapter it resolves host names with a blocking
`getaddrinfo()`.

### With GLib: the GLib adapter

`protobuf-c-grpc2-glib.h` runs servers and clients on a `GMainContext`, so a
GLib or GTK program serves and calls gRPC from its ordinary main-loop:

    o.io = protobuf_c_grpc2_io_system_new_glib(NULL);   /* the default context */
    ...
    g_main_loop_run(loop);

Sockets are `GSource`s with their descriptors added by `g_source_add_unix_fd()`,
timers and deferred completions are sources too, and the core's iteration hook
is a source whose `prepare` function runs at the start of every iteration. All
callbacks arrive from the thread that iterates the context, never from inside a
call into the library, and the I/O system, servers and clients must only be used
from that thread. The I/O system holds a reference to the context. Like the
poll and dispatch adapters it resolves host names with a blocking
`getaddrinfo()`.

The tests under `t/` that need an event loop (`endpoints`, `client`, `bulk`)
run once on each backend, through the small shim in `t/loop-backend.h`; the
dispatch adapter is tested both with `protobuf_c_rpc_dispatch_run()` and with
an embedding loop of its own (`dispatch-embed`), and the GLib adapter on a
context of its own that the test iterates step by step.
The endpoint test (a server and a client, with and without TLS) also runs once
per TLS library, through `t/tls-backend.h`, and `t/tls-interop.c` connects the
OpenSSL and GnuTLS adapters to each other in every combination.

## Threads

The library is single-threaded: everything is done from the thread that runs
the event loop. A client can opt out of that for its calls. With
`ProtobufCGrpc2ClientOptions.thread_safe` set, a service handle obtained from
`protobuf_c_grpc2_client_get_service()` may be invoked from any thread:

    o.thread_safe = 1;
    client = protobuf_c_grpc2_client_new(&o);       /* on the loop's thread */
    svc = protobuf_c_grpc2_client_get_service(client, &example__greeter__descriptor);
    ...
    /* from any thread */
    example__greeter__say_hello(svc, &req, on_reply, data);

The thread that creates the client is taken to be the loop thread. A call made
from there goes straight through. A call from another thread packs the request
(so the message may be freed or reused as soon as the call returns), posts it
to the loop with the I/O system's `post` function, which wakes the loop, and
returns; the loop starts the call. For a call with a request stream the
`ProtobufCInputStream` is returned at once, and what is sent on it is queued
behind the call, from any thread. Everything the client reports, namely
closures, `on_call_done` and the messages of streams, is delivered on the loop
thread. Failures to start a call arrive through the closure, as always, since
the caller has already returned.

`protobuf_c_grpc2_client_new()`, `_get_service()`, `_free()` and
`_service_set_timeout_ms()` stay on the loop thread; stop the other threads
before freeing the client. The I/O system must have a `post` function (the uv,
poll and GLib adapters do; the dispatch adapter cannot, because a
`ProtobufCRPCDispatch` cannot be woken from another thread), or
`protobuf_c_grpc2_client_new()` fails. Posted calls do not keep the loop
running, so keep it alive as you would to wait for a network event. A
connection accepts only a limited number of concurrent calls (the server's
`MAX_CONCURRENT_STREAMS`); calls beyond that fail with `UNAVAILABLE` rather
than wait, so many threads should bound what they have in flight.

A server can do the same for its answers. With
`ProtobufCGrpc2ServerOptions.thread_safe` set (and an I/O system with `post`),
handlers are still invoked on the loop thread, but the response closure they
are given may be called from any thread, so a handler can pass the request to a
thread pool and answer from there:

    o.thread_safe = 1;
    ...
    /* in the handler (loop thread): queue_job(req_copy, closure, closure_data); */
    /* in a worker thread: */
    closure(&reply, closure_data);   /* packed, posted to the loop, sent there */
    closure(NULL, closure_data);     /* for a response stream: the end */

A response given from another thread is copied, so the message may go as soon
as the closure returns. The usual rule about closures holds: the final call
(`NULL`, or the single response of a method without a response stream) is made
once, and the closure is not used after it; calls to one closure must not
overlap. The closure stays usable if the connection or the server is freed in
the meantime (the answer is dropped), and the I/O system is kept until the last
closure has been finished. A handler that parks a closure forever therefore
keeps the I/O system alive.

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
`SSL_VERIFY_PEER` (with GnuTLS, put the trust roots in the credentials); the server name checked (and sent as SNI) is `host` unless
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
the generated `<request>__input_stream__send()`, and ends the stream, which
also releases it, by sending NULL (or with `protobuf_c_input_stream_send()` on
its `base` member, which checks the message type at run time).
Stream implementations set `input_descriptor` to the request type. Errors are reported through the
`on_call_done` option.

## Stress testing

`stress/stress.c` forks a server and N clients on one machine. Each client
keeps a fixed number of requests in flight and stops after a given number of
them have been answered; the program prints throughput and latency and exits
nonzero if any call failed or went missing. It is built once per I/O backend
(`stress-uv`, `stress-poll`, `stress-dispatch`, `stress-dispatch-embed`, `stress-glib`) by CMake and by `make check`, which also runs a
short run of every method (`stress/smoke.sh`).

    stress-poll -n 8 -p 50 -r 5000 -m mixed -d 1

* `-n` clients (default 1), `-p` requests in flight per client (10),
  `-r` responses each client waits for (1000), `-P` port (50099).
* `-m` picks the method: `noop` (answers at once), `spin` (the server
  busy-waits `-d` milliseconds), `delay` (the server answers after `-d`
  milliseconds from a timer), `echo` (a bidirectional stream of `-c`
  messages, each answered), or `mixed` (all of them in turn).

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
