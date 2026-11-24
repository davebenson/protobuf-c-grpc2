#!/bin/sh
# Start libuv-server, run libuv-client against it, and check the output;
# first in cleartext, then (if the openssl tool and TLS support are
# available) over TLS with a throwaway certificate.
# usage: smoke-test.sh [/path/to/libuv-server /path/to/libuv-client]
# (defaults are relative to the current directory, as in `make check`)
server=${1:-./examples/libuv-server}
client=${2:-./examples/libuv-client}
dir=$(mktemp -d)
out=$dir/out
slog=$dir/server.log
spid=
trap 'kill $spid 2>/dev/null; wait $spid 2>/dev/null; rm -rf "$dir"' EXIT

# run_case LABEL PORT SERVER_EXTRA_ARGS... -- CLIENT_EXTRA_ARGS...
run_case() {
	label=$1; port=$2; shift 2
	sargs=
	while [ $# -gt 0 ] && [ "$1" != "--" ]; do sargs="$sargs $1"; shift; done
	[ "$1" = "--" ] && shift

	# shellcheck disable=SC2086
	"$server" "$port" $sargs 2>"$slog" &
	spid=$!
	sleep 0.3
	if ! kill -0 $spid 2>/dev/null; then
		echo "smoke-test: $label: server did not start: $(cat "$slog")" >&2
		spid=
		return 2
	fi

	# the server needs a moment to start listening
	ok=
	for i in 1 2 3 4 5 6 7 8 9 10; do
		if "$client" localhost "$port" world "$@" >"$out" 2>&1; then
			ok=1
			break
		fi
		kill -0 $spid 2>/dev/null || break
		sleep 0.3
	done
	cat "$out"
	kill $spid 2>/dev/null; wait $spid 2>/dev/null; spid=
	[ -n "$ok" ] || { echo "smoke-test: $label: client failed" >&2; return 1; }

	for want in \
		'SayHello reply: Hello, world!' \
		'SayHelloMany reply: Hello #3, world!' \
		'Collect reply: Hello, alice, bob, carol!' \
		'SayHello finished: OK' \
		'SayHelloMany finished: OK' \
		'Collect finished: OK'
	do
		grep -qF "$want" "$out" || {
			echo "smoke-test: $label: missing '$want'" >&2; return 1; }
	done
	echo "smoke-test: $label ok"
}

base=$((20000 + $$ % 20000))
run_case cleartext $base -- || exit 1

if command -v openssl >/dev/null 2>&1 &&
   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
	-keyout "$dir/key.pem" -out "$dir/cert.pem" -days 1 -subj /CN=localhost \
	-addext subjectAltName=DNS:localhost >/dev/null 2>&1; then
	run_case tls $((base + 1)) "$dir/cert.pem" "$dir/key.pem" -- "$dir/cert.pem"
	rc=$?
	if [ $rc -eq 2 ]; then
		echo "smoke-test: TLS skipped (built without OpenSSL?)"
	elif [ $rc -ne 0 ]; then
		exit 1
	fi
else
	echo "smoke-test: TLS skipped (no openssl tool)"
fi
echo "smoke-test: ok"
