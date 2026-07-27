# zenith — Architecture (roadmap)

Two-tier design. The ESP32 ships first and runs standalone; a Raspberry Pi is the
**eventual** second tier on top for higher-level control and real-time streaming.

---

## The stack

```
  ┌───────────────────────────┐
  │  Raspberry Pi (brain)     │  UI, logging, brew profiles, WiFi,
  │  Linux, heavy compute     │  optional camera, web dashboard
  └──────────┬────────────────┘
             │  LINK  (pick one)
             │  a) USB cable  (Pi USB -> ESP USB)  -- simplest, also powers ESP
             │  b) UART  (Pi TX/RX <-> ESP RX/TX + common GND, both 3.3V)
             │  c) WiFi/LAN  (no wire -- MQTT/WebSocket over network)
  ┌──────────┴────────────────┐
  │  ESP32 (real-time)        │  drives SSRs, reads sensors,
  │  deterministic loop       │  PID, safety loops, watchdog
  └───┬────┬────┬────┬────────┘
   heater pump grind cover  + NTC / float / flow / limit
```

## The split (hard boundary)

- **ESP32 = real-time controller.** Fixed-interval loop: read sensors, run PID,
  drive the SSRs, enforce safety (dry-pump guard, over-temp, command watchdog).
  **Must run standalone** — the Pi can vanish and the machine stays safe.
- **Pi = supervisor / brain.** UI, data logging, brew profiles, WiFi, heavy
  compute, optional camera. Sends **targets**, not raw switch commands. Reads
  back telemetry.

Link starts as **(a) USB serial** (one cable, Pi powers ESP) or **(c) pure WiFi**
if the boards live apart. It is *not* a literal HAT stack — two separate boards,
one link.

---

## Real-time streamed control (eventual)

Beyond static setpoints ("hold 94°C"), the plan is a live, time-varying feed in
both directions:

- **Telemetry OUT** (ESP -> Pi/UI, ~5–20 Hz): temp, flow (mL), load states,
  cover position -> live graph while brewing.
- **Command / profile IN** (Pi/UI -> ESP): live adjustment during the brew, or a
  streamed **brew profile** = a curve of setpoints over time (temp / flow vs
  seconds, espresso-profiling style).

**Transport** — persistent stream, not HTTP polling:

| Option | Fit |
|--------|-----|
| **MQTT** (broker on Pi; ESP publishes telemetry, subscribes to commands) | best — pub/sub, low overhead, topics like `zenith/temp`, `zenith/cmd` |
| **WebSocket** (ESP serves, browser connects) | live dashboard with sliders, no broker |
| **UART framed packets** | if the wired link is used |

---

## The safety contract (non-negotiable)

- **The real-time control loop lives on the ESP32 — never across WiFi.** The
  stream feeds *targets*; the ESP closes the loop locally at fixed interval.
  Network jitter/dropout must never stall the heater loop.
- Streamed profile example: Pi streams "t=8s, target 92°C, flow 2 mL/s" -> ESP
  interpolates and hits it with its local PID.
- **Stream drops -> command watchdog fires -> ESP fails safe** (loads off / hold).
  Watchdog is already in the firmware plan (15s timeout).

---

## Status

- **Now:** ESP32 tier — wiring + standalone firmware (WiFi web UI + OTA).
- **Eventual:** Raspberry Pi on top for logging, brew profiles, and the real-time
  streamed-control layer above.
