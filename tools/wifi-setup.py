#!/usr/bin/env python3
"""Join the ESP32 to a WiFi network and report whether it worked.

Sends the credentials as a single write, so nothing depends on how fast
anyone types. The password is prompted locally, never echoed, never stored,
and never printed back -- the board only reports its length.

    python tools/wifi-setup.py [ssid] [--port /dev/ttyUSB0]
"""
import argparse
import getpass
import sys
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("ssid", nargs="?", default="Serendipity")
ap.add_argument("--port", default="/dev/ttyUSB0")
ap.add_argument("--wait", type=float, default=30.0, help="seconds to watch for the result")
args = ap.parse_args()

pw = getpass.getpass(f'password for "{args.ssid}": ')
if not pw:
    sys.exit("no password given, aborting")
if "/" in args.ssid:
    sys.exit("ssid contains '/', which the board's parser splits on -- tell Claude")

port = serial.Serial(args.port, 115200, timeout=0.3)
port.setDTR(False)
port.setRTS(True)
time.sleep(0.1)
port.setRTS(False)          # pulse EN for a clean boot
time.sleep(6)               # let it finish booting and its own join attempt
port.reset_input_buffer()

port.write(f"W{args.ssid}/{pw}\n".encode())
port.flush()
del pw

print(f'sent credentials for "{args.ssid}", watching for the result...\n')
deadline = time.time() + args.wait
connected = False
while time.time() < deadline:
    chunk = port.read(4096).decode("utf-8", "replace")
    if not chunk:
        continue
    for line in chunk.splitlines():
        if any(k in line for k in ("joining", "saved network", "WiFi:", "mDNS:",
                                   "OTA:", "AP MODE", "IP =")):
            print("  " + line.strip())
        if "IP =" in line:
            connected = True
            deadline = min(deadline, time.time() + 3)
port.close()

print("\n" + ("CONNECTED -- OTA should be armed." if connected else
              "NOT connected. Check the password, or the board fell back to AP mode."))
sys.exit(0 if connected else 1)
