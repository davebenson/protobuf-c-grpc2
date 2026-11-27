#!/bin/sh
# Run each given stress binary (default: the ones built in ./stress) for every
# method with a few clients, and fail if any run does.
# usage: smoke.sh [stress-binary ...]
if [ $# -eq 0 ]; then
	for b in ./stress/stress-uv ./stress/stress-poll \
	    ./stress/stress-dispatch ./stress/stress-dispatch-embed; do
		[ -x "$b" ] && set -- "$@" "$b"
	done
fi
[ $# -gt 0 ] || { echo "smoke.sh: no stress binaries" >&2; exit 77; }
port=${STRESS_PORT:-50161}
status=0
for bin in "$@"; do
	for method in noop spin delay echo mixed; do
		"$bin" -n 3 -p 8 -r 300 -d 1 -m "$method" -P "$port" -t 60 \
			|| status=1
		port=$((port + 1))	# no clash with sockets left in TIME_WAIT
	done
done
exit $status
