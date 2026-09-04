#!/usr/bin/env bash
# Build the checked-out tree on CI, then install it on the board.
#
#   tools/ota.sh                 # board on this LAN
#   tools/ota.sh --remote        # board reachable over the tailnet link
#
# Whatever is committed locally is published to the build branch, which is the
# only branch CI watches -- master is deliberately excluded so that merging never
# kicks off a build. The run is then matched by commit sha rather than by "most
# recent", because picking the latest run once grabbed a different commit's
# artifact and flashed the wrong firmware.
set -euo pipefail
cd "$(dirname "$0")/.."

[ -f "$(dirname "$0")/link.env" ] && . "$(dirname "$0")/link.env"

BUILD_BRANCH="${ZENITH_BUILD_BRANCH:-build}"
HOST="http://zenith.local"
[[ "${1:-}" == "--remote" ]] && HOST="${ZENITH_REMOTE_URL:?set ZENITH_REMOTE_URL in tools/link.env}"

SHA=$(git rev-parse HEAD)
if [ -n "$(git status --porcelain)" ]; then
  echo "working tree is dirty -- commit before flashing, or you will not know what is on the board" >&2
  exit 1
fi

echo "publishing $(git rev-parse --short HEAD) to $BUILD_BRANCH"
git push -f -q origin "HEAD:$BUILD_BRANCH"

echo "waiting for the CI run for this commit..."
RID=""
for _ in $(seq 1 60); do
  RID=$(gh run list --branch "$BUILD_BRANCH" --limit 10 \
        --json databaseId,headSha -q ".[] | select(.headSha==\"$SHA\") | .databaseId" | head -1)
  [ -n "$RID" ] && break
  sleep 5
done
[ -z "$RID" ] && { echo "no CI run appeared for $SHA" >&2; exit 1; }

gh run watch "$RID" --exit-status >/dev/null
rm -rf build && mkdir -p build
gh run download "$RID" -n firmware -D build

MD5=$(md5sum build/firmware.bin | cut -d' ' -f1)
echo "firmware md5 $MD5"

stamp() { curl -s --max-time 15 "$HOST/status" 2>/dev/null | grep -o '"fw":"[^"]*"' || true; }
BEFORE=$(stamp)
echo "before: ${BEFORE:-unreachable}"

# Serve the image on the LAN so the board can pull it. The server must outlive
# the download: the board keeps answering /status while it fetches and only
# reboots at the end, so treating a reachable /status as success killed the
# transfer half way through.
python3 -m http.server 8000 --directory build --bind 0.0.0.0 >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1

LANIP=$(ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -1)
curl -s --max-time 20 "$HOST/pull?url=http://$LANIP:8000/firmware.bin" || true

# Success is the build stamp changing, not the board merely answering.
for _ in $(seq 1 40); do
  sleep 5
  NOW=$(stamp)
  if [[ -n "$NOW" && "$NOW" != "$BEFORE" ]]; then
    echo "after:  $NOW"
    exit 0
  fi
done
echo "firmware stamp never changed -- update did not land" >&2
exit 1
