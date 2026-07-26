# zenith — Findings

Cuisinart DGB-30 single-cup grind-and-brew → ESP32 takeover.
Everything we've *pinned* down (pun intended). Confidence tagged per item.
Last updated: 2026-07-15

Legend: ✅ confirmed · 🟡 strong guess · ❓ unknown / needs bench test

---

## Goal
Full control over every brew variable: brew temp, pump dose/flow, grinder dose.
ESP32 becomes the brain; factory MCU benched (drive lines cut), ESP drives the
factory relays. No triac for now → pump speed stays fixed, but flow *measurement*
(see flow sensor) lets us dose by volume.

## Architecture ✅
- Keep factory MCU physically in place, but sever its 3 relay-drive lines and
  inject ESP GPIO instead.
- ESP reads sensors, drives 3 loads, exposes WiFi control + OTA.
- Higher-level brain (Pi/phone) optional, sits on top over WiFi. ESP runs safe alone.

---

## Loads (outputs) — the 3 knobs
| Load | Board label | ESP pin | Notes |
|------|-------------|---------|-------|
| Heater | relay (big, red wire to metal base) ✅ | GPIO25 | slow-PWM for temp control |
| Pump motor | acmot1 or acmot2 ❓ | GPIO26 | which acmot is pump vs grinder TBD (settle in firmware) |
| Grinder motor | acmot1 or acmot2 ❓ | GPIO27 | " |
| Steam gate | none / rides grinder 🟡 | — | metal plate over grind chute; likely mechanical or solenoid-on-grinder. Not a separate axis. |

Mains: **acl / acn** = Line / Neutral ("wire to wall").
Board has 3 relays total (2 heater-related, per red/blue-wire-to-metal-base notes).

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
- Has: 3 loads off-at-boot, NTC analog read, float + dry-pump guard, WiFi web UI,
  `/status` JSON, OTA, 15s command watchdog. Untested, not flashed.
- **TODO from findings:** flow sensor = **pulse input**, needs interrupt-capable GPIO
  + counter (not analog like NTC/float). Add when flu confirmed.
- **Calibration pending:** need Ntc1 room-temp Ω value + beta to fix temp math
  (`NTC_NOMINAL` / `NTC_BETA` are placeholders 100k/3950).

---

## Parts on hand ✅
- 3× SSR-40DA (input 3–32V DC / output 24–380V AC)
- 1× ESP32-DevKitC-32E
- Resistors (~10k for NTC divider)
- DAOKAI RC snubber 5-pack (B0CR3HLH94; tuned 240V, fine for arc suppression)
- Still needed: 5V USB brick + micro-USB, screw terminals/wire, enclosure

---

## Open bench tasks (multimeter, machine UNPLUGGED)
1. **Green-tab sensor:** Ω test — drifts with heat = thermistor (Ntc2); snaps open/closed = switch/cutoff.
2. **flu (pipe, 3-wire):** confirm Hall flow — identify VCC/GND/signal, power it, spin impeller/blow through, watch signal pulse.
3. **Float (red 3-wire):** confirm open/closed logic when tank empty vs float raised; ID the 3 wires.
4. **Ntc1:** record room-temp Ω value → firmware calibration.
5. **acmot1 vs acmot2:** which is pump, which grinder (can defer to firmware — fire each, watch which spins).

## Working mode
Async: **talk → commute → bench.** Discussion happens away from machine; physical
work done solo at home later. Deliver self-contained take-home checklists, not
live step-by-step.
