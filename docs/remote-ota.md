# Remote bench workflow

How to change firmware and verify wiring without being at the machine.

## The loop

1. Wire something at the machine.
2. Work it by hand — flip the float, slide the cover, spin the impeller.
3. Ask Claude to read `/log`.

The board records its own edges, so step 2 and step 3 do **not** have to happen
at the same time. That is the whole point: whoever is at the machine and whoever
is watching no longer have to coordinate timing over text.

## Endpoints

| Endpoint | Purpose |
|---|---|
| `/status` | JSON snapshot: fw stamp, uptime, rssi, heap, both dividers, float, cover limit, flow, load states |
| `/log` | Event ring, oldest first. `?since=<seq>` returns only what is new |
| `/set?load=X&on=1` | Switch heater / pump / grinder / cover |
| `/off` | All loads off (recorded in the log) |
| `/pull?url=` | Fetch firmware from a URL and self-flash |
| `/update` | Push firmware. Requires `?md5=`, refuses anything else |
| `/wifi` | Add a network. Only scans when in AP mode or `?scan=1` |

## Reaching it

- On the same LAN: `http://zenith.local/`
- From anywhere on the tailnet: the URL in `tools/link.env` (see `link.env.example`)

The tailnet path is a `systemd --user` unit, `zenith-bridge.service`, running
`tools/zenith-link.sh` and published by `tailscale serve --http=8080`.

That script keeps `127.0.0.1:8099` pointed at the board wherever it is:

- **Board on this LAN** — a direct `socat` hop to `zenith.local:80`, re-resolved
  per connection, so a DHCP renewal changes nothing.
- **Board somewhere else** — an SSH tunnel through a phone that sits on the
  board's LAN and is reachable over the tailnet, configured in `tools/link.env`.
  It finds the board by sweeping its own subnet for a live `/status` (a full /24
  takes ~15s; the last hit is cached and tried first).

Either way the published URL is identical, so nothing downstream has to know
which case is in play.

Verified 2026-09-03 by moving both machines to a different LAN: the board came
back at a new IP and both paths worked with no changes.

**The tunnel costs you USB recovery.** With the board in one building and the
laptop in another, a firmware that will not boot cannot be fixed until they are
back together. The mandatory `?md5=` closes the failure that actually bit us,
but prefer keeping the laptop and the board in the same bag when there is real
flashing to do.

## Updating firmware

    tools/ota.sh              # same LAN
    tools/ota.sh --remote     # from anywhere on the tailnet

Builds on Actions, serves the image on the LAN, and tells the board to pull it.
Success is the build stamp changing, not the board merely answering — it keeps
serving `/status` throughout the download and only reboots at the end.

**Pull, not push.** A push truncated over a weak link once and the board flashed
a half-written image and boot-looped, recoverable only over USB. Pushes now
require a matching `?md5=` and are refused without one, and pull mode lets the
board set the pace so a bad link fails cleanly instead of corrupting.

## When it will not come back

Keep the USB cable in. If OTA ever leaves the board unbootable, `esptool`
recovers it in seconds:

    esptool --port /dev/ttyUSB0 erase-region 0xe000 0x2000
    esptool --port /dev/ttyUSB0 --baud 460800 write-flash \
      0x1000 build/bootloader.bin 0x8000 build/partitions.bin 0x10000 build/firmware.bin

The `erase-region` matters: it clears `otadata`. After a successful OTA the
bootloader is pointed at the other app slot, so writing to `0x10000` alone
appears to succeed and then boots the old firmware anyway.

If the board cannot find a known network it raises its own AP, `zenith-setup`
(password `zenith1234` — change `AP_PASS` before this lives anywhere public,
since anyone in radio range who knows it can rewrite the board's WiFi), and
serves the setup page — recovery with no cable at
all. It remembers three networks and tries each at boot.

## Known gaps

- `/set` and `/off` have **no authentication**. Harmless while nothing is
  wired to mains. Must be fixed before the heater is connected.
- GPIO35 floats and logs phantom flow pulses until the 10k pull-up is fitted.
- Float polarity is unconfirmed, so the dry-pump guard fails closed and refuses
  the pump entirely. `U` on the serial console overrides it at the bench.
