# zenith — Wiring Guide

Cuisinart DGB-30 → ESP32. Read fully before touching anything.

> ⚠️ **Machine UNPLUGGED from the wall for every wiring step.**
> Wall plug goes in **last**, only for the final live test (step 5).
> Two separate worlds: DC logic (safe) and mains AC (careful). Wire one at a time.

Fill in the `______` blanks from your own legend (your wire colors ↔ each point).

> **No snubbers on this build — don't mention them.** No RC snubbers on hand, none
> going in. Bare SSR / relay contacts everywhere. Do not re-add snubber steps to
> this guide, `findings.md`, `README.md`, or the diagrams.

---

## Pin map (matches firmware `src/main.cpp`)
| Signal | ESP pin | Notes |
|--------|---------|-------|
| Heater SSR control | GPIO25 | output |
| Pump SSR control | GPIO26 | output |
| Grinder SSR control | GPIO27 | output |
| Steam-cover control | GPIO32 | output (5V opto relay, active-LOW) |
| Cover limit switch | GPIO33 | INPUT_PULLUP |
| Ntc1 heatblock temp | GPIO34 | analog in (divider) |
| Float / level | GPIO14 | INPUT_PULLUP |
| Flow (flu) signal | GPIO35 | interrupt, pulse count |
| Green-tab sensor | GPIO39 (if thermistor) | analog in (divider) — OR digital / leave in mains if cutoff |

**Common ground:** every sensor ground, every SSR terminal-4 (−), and ESP `GND`
all tie together. One common ground or nothing triggers.

---

# PART A — Sensors → ESP  (low voltage, do FIRST, unplugged)

### A1. Ntc1 (heatblock thermistor) → GPIO34
```
ESP 3V3 ──[ 10k resistor ]──┬── GPIO34
                            │
        Ntc1 wire A ────────┘        A = ______
        Ntc1 wire B ──────────── GND  B = ______
```
NTC not polar — wire order doesn't matter.

### A2. Float (level) → GPIO14
```
Float wire 1 ── GPIO14     wire 1 = ______
Float wire 2 ── GND        wire 2 = ______
```
No resistor (firmware uses INPUT_PULLUP).
3-wire float: find the switch pair; third wire likely common/unused → cap it.
3rd wire = ______

### A3. flu (flow sensor, Hall, 3-wire) → GPIO35 (interrupt)
```
VCC wire    ── ESP 3V3   (if no pulses, move to 5V/VIN)   VCC = ______
GND wire    ── ESP GND                                    GND = ______
Signal wire ── GPIO35                                     SIG = ______
```
Optional 10k pull-up: signal wire → 3V3, if pulses weak.
Proves itself live: spin impeller / blow through pipe → count climbs.

### A4. Green-tab sensor → (depends on type — FILL IN once known)
Type = ______  (thermistor / switch / thermal-cutoff)
- If **thermistor**: divider like A1 on GPIO39.
- If **switch**: like A2 on a spare GPIO.
- If **thermal-cutoff**: leave in mains path (Part B), do NOT bring to ESP.

---

# PART B — SSRs → loads  (MAINS — careful, unplugged)

Each SSR:
- **Input/control** = terminals **3 (+)** and **4 (−)**, `3-32VDC` — from ESP.
- **Output/load** = terminals **1** and **2**, `~` high voltage — switches mains.

### B1. Control side (DC — safe)
```
ESP GPIO25 ── Heater SSR  term 3 (+)      Heater SSR term 4 (−) ── ESP GND
ESP GPIO26 ── Pump SSR    term 3 (+)      Pump SSR   term 4 (−) ── ESP GND
ESP GPIO27 ── Grinder SSR term 3 (+)      Grinder SSR term 4 (−) ── ESP GND
```
All three term-4 (−) tie to same ESP GND (common ground mandatory).

⚠️ 3.3V is the bottom edge of SSR-40DA trigger range. Usually works.
If an SSR won't switch reliably → drive its input from 5V via a small transistor.
Test first; add transistors only if needed.

### B2. Output side (MAINS — SSR in series with the LINE wire)
Put each SSR in series with the hot (Line) leg of its load. Neutral stays straight.
```
acl (Line) ── SSR term 1        SSR term 2 ── load hot terminal
acn (Neutral) ── straight to all loads (not switched)
```
Path per load: Line → SSR → load → Neutral.
Feed all three SSR term-1 from the same acl (Line) bus.

Per-load hot wire (your legend):
- Heater  hot wire = **red** (→ SSR term 2). Heater **blue** → acn (Neutral) direct. Resistive/non-polar; red chosen as switched leg per factory relay note.
- Pump    hot wire = ______
- Grinder hot wire = ______

**Factory board:** removed entirely — no factory relays left. Route each load's
hot wire straight through your SSR. Recreate the mains Line/Neutral junctions
(that used to live on the board) on a barrier strip. Keep any thermal cutoff
inline in the heater Line.

### B3. Heatsink
- Heater SSR (~15W) → bolt to heatsink OR machine metal base (thermal paste).
- Pump / grinder SSR → run cool, no heatsink.

---

# BUILD ORDER  (do not skip the sequence)
1. Wire all of **Part A** (sensors). Unplugged.
2. Wire **control side** of all 3 SSRs (**B1**). Still all DC, unplugged.
3. Power ESP from **5V USB brick only** (NOT wall mains). Flash firmware,
   open web page, tap each load button → SSR **input LEDs** light.
   Proves entire logic side with ZERO mains connected.
4. Only after step 3 passes: unplug USB, wire **mains side** (B2/B3).
   Machine still unplugged from wall.
5. Live test: plug machine into wall, water tank FULL, fire one load at a time
   from the web page.

⚠️ Never work on B2 mains wiring with the machine plugged into the wall.
Wall plug goes in LAST, step 5 only.

---

## Open items to finalize
- [ ] Green-tab sensor type (A4) — thermistor / switch / cutoff?
- [ ] Ntc1 room-temp Ω value → firmware calibration (NTC_NOMINAL / NTC_BETA)
- [ ] Confirm flu pulses when impeller spins (A3)
- [ ] Which acmot = pump vs grinder → confirm at step 5 (fire each, watch which spins)
