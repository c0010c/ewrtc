#!/usr/bin/env bash
# Use a temporary user/network namespace. No host routes or qdiscs are changed.
set -euo pipefail
script=$(realpath "$0")
if [[ ${1:-} != --inside ]]; then
  export EWRTC_HOST_NETNS=$(readlink /proc/self/ns/net)
  exec unshare --user --map-root-user --net "$script" --inside "$@"
fi
shift
video=$(realpath "${1:?Usage: run_isolated_network.sh VIDEO.frames OUTPUT_DIR}")
output=$(realpath -m "${2:?Missing output directory}")
repo=$(dirname "$(dirname "$script")")
mkdir -p "$output"
ip link set lo up
# ICE libraries need a non-loopback host interface even for a local TURN test.
ip link add rtc-test type dummy
ip addr add 192.0.2.1/24 dev rtc-test
ip link set rtc-test up
turnserver -n --no-cli --no-tcp --no-tls --no-dtls --no-tcp-relay \
  --listening-ip=127.0.0.1 --relay-ip=127.0.0.1 --realm=ewrtc.local \
  --lt-cred-mech --user=test:testpass --allow-loopback-peers \
  --listening-port=3479 --min-port=49160 --max-port=49260 \
  --pidfile="$output/turn.pid" --log-file="$output/turn.log" > "$output/turn-console.log" 2>&1 &
turn_pid=$!
trap 'kill "$turn_pid" 2>/dev/null || true; wait "$turn_pid" 2>/dev/null || true' EXIT
python3 "$repo/examples/test_loss.py" --video "$video" --output-dir "$output/loss"
python3 "$repo/examples/test_recovery.py" --video "$video" --output-dir "$output/recovery"
