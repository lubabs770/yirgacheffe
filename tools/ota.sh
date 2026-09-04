#!/usr/bin/env bash
# Build on CI, then push the result to the board over the air.
#   tools/ota.sh [host]      host defaults to zenith.local, falls back to the IP
set -euo pipefail
HOST="${1:-zenith.local}"
cd "$(dirname "$0")/.."

echo "waiting for CI build of $(git rev-parse --short HEAD)..."
RID=$(gh run list --branch "$(git branch --show-current)" --limit 1 --json databaseId -q '.[0].databaseId')
gh run watch "$RID" --exit-status >/dev/null

rm -rf build && mkdir -p build
gh run download "$RID" -n firmware -D build

BEFORE=$(curl -s --max-time 5 "http://$HOST/status" | grep -o '"fw":"[^"]*"' || echo "unreachable")
echo "before: $BEFORE"

curl -s --max-time 180 -F "firmware=@build/firmware.bin" "http://$HOST/update"

for _ in $(seq 1 30); do
  R=$(curl -s --max-time 3 "http://$HOST/status" 2>/dev/null || true)
  if grep -q uptime_s <<<"$R"; then
    echo "after:  $(grep -o '"fw":"[^"]*"' <<<"$R")"
    exit 0
  fi
  sleep 4
done
echo "board did not come back within 2 minutes" >&2
exit 1
