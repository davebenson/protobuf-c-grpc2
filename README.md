# protobuf-c-grpc2

gRPC over HTTP/2 for protobuf-c, built on [nghttp2 version 2][ng2]
(`libnghttp2v2`).

This directory is a **separate package**. It has its own `configure.ac` and
`CMakeLists.txt` and builds against an *installed* `libprotobuf-c` and
`libnghttp2v2`, so protobuf-c itself has no dependency on nghttp2. It
installs `libprotobuf-c-grpc2`, `<protobuf-c-grpc2/protobuf-c-grpc2.h>` and
`libprotobuf-c-grpc2.pc`.

## Building

    cd grpc2
    autoreconf -i && ./configure && make && make check
    # or
    cmake -S . -B build && cmake --build build && ctest --test-dir build

`make check` needs `protoc` and an installed `protoc-gen-c` (`--with-protoc`,
`--with-protoc-gen-c` select them).

## Using it

The API is sans-I/O: you own the sockets (and TLS).

* Server: `grpc2_server_new()`, `grpc2_server_add_service()` with generated
  `ProtobufCService` implementations, `grpc2_conn_server_new()` per accepted
  connection.
* Client: `grpc2_conn_client_new()`, then `grpc2_conn_client_service()` to get
  a `ProtobufCService` for a generated service descriptor, and call the
  generated caller functions.
* Move bytes with `grpc2_conn_read()` and `grpc2_conn_write()`.

Streaming methods use `ProtobufCService::invoke` as documented in
`protobuf-c.h`: `PROTOBUF_C_OUTPUT_STREAMING` calls deliver many responses and
a final NULL; `PROTOBUF_C_INPUT_STREAMING` calls return a
`ProtobufCInputStream` on which the caller sends requests and ends the
stream with `send_input(NULL)`. Errors are reported through
`Grpc2ConnOptions::on_call_done`.

Limitations: no TLS/ALPN helpers, no message compression, no deadlines
(`grpc-timeout`) or metadata API yet, and no thread safety.

[ng2]: https://github.com/nghttp2/nghttp2
