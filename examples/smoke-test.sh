#!/bin/sh
# Start libuv-server, run libuv-client against it, and check the output.
# usage: smoke-test.sh [/path/to/libuv-server /path/to/libuv-client]
# (defaults are relative to the current directory, as in `make check`)
server=${1:-./examples/libuv-server}
client=${2:-./examples/libuv-client}
port=$((20000 + $$ % 20000))
out=$(mktemp)
trap 'kill $spid 2>/dev/null; wait $spid 2>/dev/null; rm -f "$out"' EXIT

"$server" "$port" 2>/dev/null &
spid=$!

# the server needs a moment to start listening
ok=
for i in 1 2 3 4 5 6 7 8 9 10; do
	if "$client" 127.0.0.1 "$port" world >"$out" 2>&1; then
		ok=1
		break
	fi
	kill -0 $spid 2>/dev/null || break
	sleep 0.3
done
cat "$out"
[ -n "$ok" ] || { echo "smoke-test: client failed" >&2; exit 1; }

for want in \
	'SayHello reply: Hello, world!' \
	'SayHelloMany reply: Hello #3, world!' \
	'Collect reply: Hello, alice, bob, carol!' \
	'SayHello finished: OK' \
	'SayHelloMany finished: OK' \
	'Collect finished: OK'
do
	grep -qF "$want" "$out" || { echo "smoke-test: missing '$want'" >&2; exit 1; }
done
echo "smoke-test: ok"
