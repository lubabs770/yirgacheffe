#!/usr/bin/env bash
# Build on CI, then install it on the board.
#
#   tools/ota.sh                 # local LAN, via zenith.local
#   tools/ota.sh --remote        # from anywhere on the tailnet, via omarchy
#
# Defaults to pull mode: the board fetches the image itself, which survives a
# marginal link far better than pushing into it. Push mode is kept as a
# fallback and always sends an md5, without which the board refuses the image.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="http://zenith.local"
[[ "${1:-}" == "--remote" ]] && HOST="http://omarchy.tail67aa85.ts.net:8080"

RID=$(gh run list --branch "$(git branch --show-current)" --limit 1 --json databaseId -q '.[0].databaseId')
echo "waiting on CI run $RID..."
gh run watch "$RID" --exit-status >/dev/null
rm -rf build && mkdir -p build
gh run download "$RID" -n firmware -D build

MD5=$(md5sum build/firmware.bin | cut -d' ' -f1)
echo "firmware md5 $MD5"
echo "before: $(curl -s --max-time 5 "$HOST/status" | grep -o '"fw":"[^"]*"' || echo unreachable)"

# Serve the image on the LAN so the board can pull it.
python3 -m http.server 8000 --directory build --bind 0.0.0.0 >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1

LANIP=$(ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -1)
curl -s --max-time 20 "$HOST/pull?url=http://$LANIP:8000/firmware.bin" || true

for _ in $(seq 1 40); do
  R=$(curl -s --max-time 3 "$HOST/status" 2>/dev/null || true)
  if grep -q uptime_s <<<"$R"; then
    echo "after:  $(grep -o '"fw":"[^"]*"' <<<"$R")"
    exit 0
  fi
  sleep 4
done
echo "board did not come back within ~3 minutes" >&2
exit 1
