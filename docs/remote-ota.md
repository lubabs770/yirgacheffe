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
- From anywhere on the tailnet: `http://omarchy.tail67aa85.ts.net:8080/`

The tailnet path is a `systemd --user` unit, `zenith-bridge.service`, running
`socat` from `127.0.0.1:8099` to `zenith.local:80`, published by
`tailscale serve --http=8080`. socat re-resolves the name on every connection,
so the board's DHCP lease can change — or the whole setup can move to another
building — without touching anything. Verified on 2026-09-03 by moving both
machines to a different LAN: the board came back at a new IP and both paths
worked with no changes.

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
(password `zenith1234`), and serves the setup page — recovery with no cable at
all. It remembers three networks and tries each at boot.

## Known gaps

- `/set` and `/off` have **no authentication**. Harmless while nothing is
  wired to mains. Must be fixed before the heater is connected.
- GPIO35 floats and logs phantom flow pulses until the 10k pull-up is fitted.
- Float polarity is unconfirmed, so the dry-pump guard fails closed and refuses
  the pump entirely. `U` on the serial console overrides it at the bench.
