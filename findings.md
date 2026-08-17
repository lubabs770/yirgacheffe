# zenith — Findings

Cuisinart DGB-30 single-cup grind-and-brew → ESP32 takeover.
Everything we've *pinned* down (pun intended). Confidence tagged per item.
Last updated: 2026-08-04

Legend: ✅ confirmed · 🟡 strong guess · ❓ unknown / needs bench test

---

## Goal
Full control over every brew variable: brew temp, pump dose/flow, grinder dose.
ESP32 becomes the brain. The **factory control board has been removed entirely** —
the ESP drives new SSRs directly, no factory electronics left in the loop. No
triac for now → pump speed stays fixed, but flow *measurement* (see flow sensor)
lets us dose by volume.

## Architecture ✅
- **Factory board removed** (not bypassed — physically gone). ESP replaces it
  wholesale.
- SSRs replace the factory relays; an in-machine AC-DC adapter replaces the
  board's power supply; mains (acl/acn) redistributed on a barrier strip.
- Any thermal cutoff stays inline in the mains safety path (off-board).
- ESP reads sensors, drives 4 loads (heater, pump, grinder, steam cover),
  exposes WiFi control + OTA.
- Higher-level brain (Pi/phone) optional, sits on top over WiFi. ESP runs safe alone.

---

## Loads (outputs) — the 4 knobs
| Load | Board label | ESP pin | Notes |
|------|-------------|---------|-------|
| Heater | relay (big, red wire to metal base) ✅ | GPIO25 | slow-PWM for temp control |
| Pump motor | acmot ❓ | GPIO26 | **mains AC ✅ — 42.6 Ω, confirmed 2026-08-02** (see Resistance readings). SSR-40DA, original plan stands. Which acmot vs grinder TBD |
| Grinder motor | acmot ❓ | GPIO27 | mains AC 🟡 (the one "main AC motor" pair observed) |
| Steam cover | **motorized, AC vs DC TBD** 🟡 | GPIO32 (+ GPIO33 limit in) | **4th axis.** Slides cover open/closed over steam. red/black = motor drive; blue/blue = limit switch. **23.6 Ω (2026-08-02)** — same low-ohm regime as the AC pump, so **AC synchronous now leads** (earlier DC guess walked back). **Driver = 5V opto relay (8-pack received 2026-08-04), active-LOW, on GPIO32** — mechanical relay switches AC or DC, so it works either way and moots the spin test for *choosing a part*. Switch side: AC → mains; DC → wall wart. Bump test still tells which. Wiring sheets: `docs/cover-ac.*` (hum/nothing) and `docs/cover-dc.*` (kick), overview in `docs/cover-relay.*`. |

Mains: **acl / acn** = Line / Neutral ("wire to wall").
Board has 3 relays total (2 heater-related, per red/blue-wire-to-metal-base notes).

### Physical build state (2026-07-29)
- **3× SSR installed.** SSRs = heater, pump, grinder. Bare contacts — see the
  no-snubbers rule at the top of `wiring.md`.
- **Loose parts on bench, unidentified:** a bunch of 3-legged parts "not put in."
  If transistors (2N2222/BC547/MOSFET) = optional 5V SSR-input drivers (only if
  3.3V won't trigger).
- **Steam cover — AC vs DC still open (23.6 Ω).** Earlier "almost certainly DC"
  guess is WALKED BACK: 23.6 Ω is the same low-ohm regime as the confirmed-AC
  pump (42.6 Ω), and the factory board switched everything with mechanical relays
  (AC topology, no MOSFET drivers). AC synchronous now leads. Settle with the
  spin-shaft test: Ω jumps as you rotate by hand = brushed DC; dead steady = AC coil.
- **Layout:** AC wiring lives on the bottom; a hot-glued wall adapter down there
  sends **USB-C up to the top (near grinder)** alongside all DC/low-voltage — clean
  AC-bottom / DC-top split.
- **SSR mounting:** 2 SSRs in the OG MCU enclosure; 3rd hot-glued in the opposing
  leg of the adapter. Heater SSR (+1 other) screwed to the OG metal plate cover =
  heatsink, with vents aligned to the OG plastic casing.

### Resistance readings + factory-board photos (2026-08-02)
Machine unplugged. Meter across each motor's own wire pair:
- **Water pump = 42.6 Ω → mains AC (shaded-pole).** In the 20–80 Ω band; too high
  for a low-V DC pump (single digits), too low for an Ulka solenoid (100–400 Ω).
  **Resolves the "pump might be DC" audit — it's AC.** Original SSR-40DA plan holds:
  no MOSFET, no DC rail, no PWM flow. Fixed speed, dose by volume via `flu`.
- **Steam cover motor = 23.6 Ω → inconclusive, leans AC.** Same regime as the AC
  pump; not the single-digit reading a small DC motor gives. Spin-shaft test to confirm.

**Factory board is NOT scrapped — photographed in-hand (`~/Downloads/coffee*.jpeg`).**
Board decode (settles bench task 8 without touching the pump):
- **AC switching topology:** black relay cubes + **yellow X-caps** across the
  contacts = mains inductive loads. No TO-220 MOSFET/flyback drivers anywhere.
- **PSU:** `EE19-0.8mH` SMPS transformer + `+12V / +5V / GND` rails (silkscreen on
  back) = **logic + relay-coil supply only**, not a motor drive. So the 12V rail
  does NOT imply a DC pump.

### Top-casing teardown finds (2026-07-26)
Removed top casing (screws on top).
- **Steam cover is motorized** — reclassified from passive gate to a 4th
  controllable axis. "Funny motor": likely AC synchronous gearmotor (one-way,
  cam + limit switch) — confirm type at bench.
  - red/black pair = motor winding (steady Ω).
  - blue/blue pair = limit/position switch (flips 0↔OL when cover moved by hand).
- Main AC motor uses a **red/black** wire pair (which of pump/grinder = TBD).
- **4-white-wire "safety" = thermal cutoff / high-limit — KEEP.** Wire in series
  in heater mains Line leg, upstream of heater SSR. Hardware backstop; do not junk.
- 3-wire to button PCB = factory UI, junk OK (ESP replaces).

---

## Sensors (inputs)
| Physical connector | Function | Board label | Wires | Confidence |
|--------------------|----------|-------------|-------|-----------|
| White tab → thermal block | **Heatblock temp** (control sensor) | Ntc1 | 2× black | ✅ thermistor |
| White tab, 3-wire → **pipe** connection at pump | **FLOW sensor** (Hall, pulse) | flu ("flow", phonetic) | black/white/red | 🟡 strong — 3-wire on a pipe = classic Hall flow |
| Red-ish tab, 3-wire → white tab at back, lines up with plastic float in tank | **Water level / float** | cn1 / Not1 | 2× white + purple | 🟡 strong — physical alignment with float |
| Green tab → top of water pump | Boiler/pump temp OR thermal cutoff | Ntc2 / sw | black + red | ❓ needs meter |

### Key reinterpretation (2026-07-15)
`flu` = **FLOW**, not float. Reasons: phonetic silkscreen; sits inline on a pipe;
3-wire = Hall flow sensor signature (VCC/GND/pulse). The *actual* float is the
3-wire red tab that lines up with the tank float. This is the upgrade we wanted —
flow lets us **dose by volume** (run pump until X mL passed) even without a triac.

### Board bottom cluster (6 connectors) reference
`Ntc1, Ntc2, sw2 / Not1, sw1, cn1` = 2 thermistors + 2 switches + 2 aux.
Switches (sw1/sw2) were factory buttons/interlocks — not needed, ESP replaces the UI.

---

## Must-haves vs bonus
- **Must-have for control:** Ntc1 (temp) + float. That's it.
- **Big bonus:** flow sensor (flu) → volumetric dosing. Worth wiring.
- **Leave alone:** any thermal cutoff/fuse (keep in mains safety path, not ESP).
- **Ignore:** sw1/sw2 (factory buttons), cn1/Not1 if they turn out unused.

---

## Firmware
- PlatformIO skeleton planned (`platformio.ini` + `src/main.cpp`) — not yet committed here.
- Skeleton had: 3 loads off-at-boot, NTC analog read, float + dry-pump guard,
  WiFi web UI, `/status` JSON, OTA, 15s command watchdog. Untested, not flashed.
- **Add 4th load** (steam cover, GPIO32) + cover limit-switch input (GPIO33).
- **TODO from findings:** flow sensor = **pulse input**, needs interrupt-capable GPIO
  + counter (not analog like NTC/float). Add when flu confirmed.
- **Calibration pending:** need Ntc1 room-temp Ω value + beta to fix temp math
  (`NTC_NOMINAL` / `NTC_BETA` are placeholders 100k/3950).

---

## Parts on hand ✅
- 3× SSR-40DA (input 3–32V DC / output 24–380V AC)
- 1× ESP32-DevKitC-32E
- 10k resistors — **received 2026-08-04** (NTC dividers; one per thermistor; see `docs/ntc-divider.*`)
- **5V opto-isolated relay module, 8-pack — received 2026-08-04** (SRD-05VDC-SL-C, 1-ch, NO, 10A@250VAC / 30VDC, active-LOW). Steam-cover driver on GPIO32; 7 spare.
- Still needed: 5V USB brick + micro-USB, screw terminals/wire, enclosure. Wall wart for cover *only if* it meters out DC.

---

## Open bench tasks (multimeter, machine UNPLUGGED)
1. **Green-tab sensor:** Ω test — drifts with heat = thermistor (Ntc2); snaps open/closed = switch/cutoff.
2. **flu (pipe, 3-wire):** confirm Hall flow — identify VCC/GND/signal, power it, spin impeller/blow through, watch signal pulse.
3. **Float (red 3-wire):** confirm open/closed logic when tank empty vs float raised; ID the 3 wires.
4. **Ntc1:** record room-temp Ω value → firmware calibration.
5. **acmot1 vs acmot2:** which is pump, which grinder (can defer to firmware — fire each, watch which spins).
6. **Steam cover motor:** confirm red/black=winding vs blue/blue=switch (Ω per pair; switch flips when cover moved by hand). Classify motor AC vs DC (winding Ω + markings) → sets whether the cover-relay switch side goes to **mains (AC)** or a **wall wart (DC)**; driver part already chosen (5V opto relay, switches either).
7. **4-white safety cutoff:** confirm it's thermal (high-limit) → keep in series in heater mains Line, upstream of heater SSR.

## Working mode
Async: **talk → commute → bench.** Discussion happens away from machine; physical
work done solo at home later. Deliver self-contained take-home checklists, not
live step-by-step.
