#!/usr/bin/env bash
# Keeps 127.0.0.1:$PORT pointing at the ESP32, wherever it currently is.
#
# The board and this laptop are not always on the same network. When they are,
# this is a direct socat hop. When they are not, it tunnels through the Moto,
# which sits on the board's home LAN and is reachable over the tailnet. Callers
# -- including `tailscale serve` -- see the same local port either way, so the
# published URL never changes.
set -uo pipefail

PORT="${ZENITH_PORT:-8099}"
PHONE_USER="${PHONE_USER:-u0_a168}"
PHONE_HOST="${PHONE_HOST:-100.79.207.105}"
PHONE_PORT="${PHONE_PORT:-8022}"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/zenith-last-ip"

log() { echo "[zenith-link] $*" >&2; }

phone_ssh() {
  timeout 45 ssh -o BatchMode=yes -o ConnectTimeout=10 \
    -o ServerAliveInterval=15 -o ServerAliveCountMax=3 \
    -p "$PHONE_PORT" "$PHONE_USER@$PHONE_HOST" "$@"
}

# Ask the phone to find the board on its own LAN. A cached hit is checked first,
# because a full sweep costs ~10s and the address rarely changes.
discover_via_phone() {
  local last subnet
  last=$(cat "$CACHE" 2>/dev/null || true)

  read -r -d '' probe <<PROBE || true
last="$last"
if [ -n "\$last" ] && curl -s -m 2 "http://\$last/status" | grep -q uptime_s; then
  echo "\$last"; exit 0
fi
ip=\$(termux-wifi-connectioninfo 2>/dev/null | awk -F'"' '/"ip"/{print \$4}')
[ -z "\$ip" ] && exit 1
subnet=\${ip%.*}
seq 1 254 | xargs -P 40 -I{} sh -c \
  'curl -s -m 2 "http://'"\$subnet"'.{}/status" 2>/dev/null | grep -q uptime_s && echo '"\$subnet"'.{}' \
  | head -1
PROBE

  phone_ssh "$probe" 2>/dev/null | tr -d '\r' | grep -oE '^[0-9.]+$' | head -1
}

while true; do
  if getent hosts zenith.local >/dev/null 2>&1; then
    log "board is on this LAN, direct hop"
    socat "TCP-LISTEN:$PORT,fork,reuseaddr,bind=127.0.0.1" TCP:zenith.local:80
    log "direct hop ended"
  else
    log "board not on this LAN, looking for it via the phone"
    IP=$(discover_via_phone)
    if [ -z "$IP" ]; then
      log "phone could not find it; retrying in 30s"
      sleep 30
      continue
    fi
    mkdir -p "$(dirname "$CACHE")"; echo "$IP" > "$CACHE"
    log "found at $IP, tunnelling through the phone"
    timeout 3600 ssh -o BatchMode=yes -o ExitOnForwardFailure=yes \
      -o ServerAliveInterval=15 -o ServerAliveCountMax=3 \
      -p "$PHONE_PORT" -N -L "$PORT:$IP:80" "$PHONE_USER@$PHONE_HOST"
    log "tunnel ended"
  fi
  sleep 5
done
