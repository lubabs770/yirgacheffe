// zenith — Cuisinart DGB-30 ESP32 takeover
// BENCH FIRMWARE: characterization + manual load control, serial + WiFi/OTA.
//
// Purpose (build-order step 3): prove the whole logic side with ZERO mains
// connected, and identify the still-unknown sensors without a multimeter.
//
// OTA is the point of this build. Once the machine is closed up, the USB port
// is behind fragile, obstructed joints — so every later firmware change has to
// arrive over the air. Prove OTA works BEFORE anything gets buttoned up.
//
// WiFi credentials are never in this repo. Set them once over serial with
//     W<ssid>/<password>
// and they persist in NVS across reboots and OTA pushes.

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ---------------------------------------------------------------- pin map
// Matches wiring.md. GPIO34/35/39 are input-only (no internal pullup).
static const int PIN_HEATER      = 25;  // SSR-40DA, active-HIGH
static const int PIN_PUMP        = 26;  // SSR-40DA, active-HIGH
static const int PIN_GRINDER     = 27;  // SSR-40DA, active-HIGH
static const int PIN_COVER       = 32;  // 5V opto relay module, ACTIVE-LOW
static const int PIN_COVER_LIMIT = 33;  // blue/blue limit switch, INPUT_PULLUP
static const int PIN_NTC1        = 34;  // heatblock thermistor, 10k divider
static const int PIN_FLOAT       = 14;  // tank float switch, INPUT_PULLUP
static const int PIN_FLOW        = 35;  // flu, Hall pulse, interrupt
static const int PIN_GREEN       = 39;  // green-tab, TYPE UNKNOWN (see notes)

// ------------------------------------------------------------- divider math
// 3V3 --[ 10k ]--+-- ADC pin
//                +-- sensor -- GND      =>  Rsensor = Rfixed * V / (Vsup - V)
static const float R_FIXED     = 10000.0f;
static const float VSUPPLY_MV  = 3300.0f;

// ---------------------------------------------------------------- safety
static const uint32_t CMD_WATCHDOG_MS = 15000;  // any load auto-offs if idle
static const uint32_t TELEMETRY_MS    = 500;

// Float polarity is OPEN BENCH TASK 3 — not yet confirmed which level means
// "tank has water". Until characterized, the dry-pump guard refuses to run the
// pump at all unless UNSAFE_ALLOW_DRY_PUMP is set at runtime ('U' command).
// There is no mains at step 3, so this costs nothing now and protects step 5.
static const int  FLOAT_WET_LEVEL     = LOW;
static const bool FLOAT_POLARITY_KNOWN = false;

struct Load {
  const char  *name;
  int          pin;
  bool         activeLow;
  bool         on;
  uint32_t     lastCmdMs;
};

static Load loads[] = {
  {"heater",  PIN_HEATER,  false, false, 0},
  {"pump",    PIN_PUMP,    false, false, 0},
  {"grinder", PIN_GRINDER, false, false, 0},
  {"cover",   PIN_COVER,   true,  false, 0},
};
static const size_t N_LOADS = sizeof(loads) / sizeof(loads[0]);

static Preferences prefs;
static WebServer    server(80);
static bool         wifiUp = false;
static bool         otaUp  = false;

static volatile uint32_t flowPulses = 0;
static uint32_t          lastFlowSnapshot = 0;
static uint32_t          lastTelemetryMs  = 0;
static bool              unsafeAllowDryPump = false;

static void IRAM_ATTR onFlowPulse() { flowPulses++; }

// Drive one load to a state, honoring its polarity.
static void applyLoad(Load &l) {
  digitalWrite(l.pin, (l.on != l.activeLow) ? HIGH : LOW);
}

static void allOff() {
  for (size_t i = 0; i < N_LOADS; i++) {
    loads[i].on = false;
    applyLoad(loads[i]);
  }
}

static bool tankHasWater() {
  if (!FLOAT_POLARITY_KNOWN) return false;   // fail closed until confirmed
  return digitalRead(PIN_FLOAT) == FLOAT_WET_LEVEL;
}

static void setLoad(size_t i, bool on) {
  Load &l = loads[i];

  // Dry-pump guard: the pump is the only load that can destroy itself running
  // empty. Heater is guarded by the factory thermal cutoff still inline.
  if (on && l.pin == PIN_PUMP && !tankHasWater() && !unsafeAllowDryPump) {
    Serial.println(F("REFUSED pump: dry-pump guard (float unconfirmed or tank empty). 'U' to override at bench."));
    return;
  }

  l.on = on;
  l.lastCmdMs = millis();
  applyLoad(l);
  Serial.printf("%s = %s\n", l.name, on ? "ON" : "off");
}

// Read a divider pin and report both the raw millivolts and the implied
// sensor resistance. Uses eFuse-calibrated mV, not the raw nonlinear count.
static float dividerOhms(int pin, uint32_t &mvOut) {
  mvOut = analogReadMilliVolts(pin);
  float v = (float)mvOut;
  if (v >= VSUPPLY_MV - 1.0f) return -1.0f;   // open circuit / rail-pinned
  return R_FIXED * v / (VSUPPLY_MV - v);
}

static void printHelp() {
  Serial.println(F("\n--- zenith bench console ---"));
  Serial.println(F("  h/p/g/c : toggle heater / pump / grinder / cover"));
  Serial.println(F("  0       : ALL LOADS OFF"));
  Serial.println(F("  s       : print one status line"));
  Serial.println(F("  z       : zero the flow pulse counter"));
  Serial.println(F("  U       : arm dry-pump override (bench only, no mains)"));
  Serial.println(F("  W..     : set wifi, W<ssid>/<password> (saved, reboots)"));
  Serial.println(F("  i       : wifi/OTA/IP info"));
  Serial.println(F("  ?       : this help"));
  Serial.println(F("Loads auto-off after 15s with no command (watchdog).\n"));
}

static void telemetry() {
  uint32_t ntcMv = 0, greenMv = 0;
  float ntcOhms   = dividerOhms(PIN_NTC1, ntcMv);
  float greenOhms = dividerOhms(PIN_GREEN, greenMv);

  uint32_t pulses = flowPulses;
  uint32_t dPulses = pulses - lastFlowSnapshot;
  lastFlowSnapshot = pulses;

  Serial.printf(
    "ntc1=%4umV/%8.1fohm  green=%4umV/%8.1fohm  float=%s  coverLimit=%s  flow=%lu(+%lu)  loads[H%d P%d G%d C%d]\n",
    ntcMv, ntcOhms, greenMv, greenOhms,
    digitalRead(PIN_FLOAT) ? "HIGH" : "LOW ",
    digitalRead(PIN_COVER_LIMIT) ? "HIGH" : "LOW ",
    (unsigned long)pulses, (unsigned long)dPulses,
    loads[0].on, loads[1].on, loads[2].on, loads[3].on);
}


// ------------------------------------------------------------------ WiFi/OTA
static void startWifi() {
  prefs.begin("zenith", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.isEmpty()) {
    Serial.println(F("WiFi: no credentials stored. Set with  W<ssid>/<password>"));
    return;
  }

  Serial.printf("WiFi: connecting to %s ...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("zenith");
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi: FAILED. Serial still works; fix creds and reboot."));
    return;
  }
  wifiUp = true;
  Serial.print(F("WiFi: connected, IP = "));
  Serial.println(WiFi.localIP());

  if (MDNS.begin("zenith")) Serial.println(F("mDNS: http://zenith.local/"));

  // Loads must never be energized across a firmware swap.
  ArduinoOTA.setHostname("zenith");
  ArduinoOTA.onStart([]() { allOff(); Serial.println(F("OTA: start, all loads off")); });
  ArduinoOTA.onEnd([]()   { Serial.println(F("OTA: done, rebooting")); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA: error %u\n", e); });
  ArduinoOTA.begin();
  otaUp = true;
  Serial.println(F("OTA: armed"));
}

static void saveWifi(const String &arg) {
  int slash = arg.indexOf('/');
  if (slash < 1) { Serial.println(F("usage: W<ssid>/<password>")); return; }
  prefs.begin("zenith", false);
  prefs.putString("ssid", arg.substring(0, slash));
  prefs.putString("pass", arg.substring(slash + 1));
  prefs.end();
  Serial.println(F("WiFi credentials saved. Rebooting..."));
  delay(300);
  ESP.restart();
}

static String statusJson() {
  uint32_t ntcMv = 0, greenMv = 0;
  float ntcOhms   = dividerOhms(PIN_NTC1, ntcMv);
  float greenOhms = dividerOhms(PIN_GREEN, greenMv);
  String j = "{";
  j += "\"ntc1_mv\":" + String(ntcMv) + ",\"ntc1_ohm\":" + String(ntcOhms, 1);
  j += ",\"green_mv\":" + String(greenMv) + ",\"green_ohm\":" + String(greenOhms, 1);
  j += ",\"float\":" + String(digitalRead(PIN_FLOAT));
  j += ",\"cover_limit\":" + String(digitalRead(PIN_COVER_LIMIT));
  j += ",\"flow\":" + String((unsigned long)flowPulses);
  j += ",\"loads\":{";
  for (size_t i = 0; i < N_LOADS; i++) {
    j += "\"" + String(loads[i].name) + "\":" + String(loads[i].on ? 1 : 0);
    if (i + 1 < N_LOADS) j += ",";
  }
  j += "}}";
  return j;
}

static void startWeb() {
  server.on("/status", []() { server.send(200, "application/json", statusJson()); });

  server.on("/set", []() {
    String name = server.arg("load");
    bool on = server.arg("on") == "1";
    for (size_t i = 0; i < N_LOADS; i++) {
      if (name == loads[i].name) { setLoad(i, on); break; }
    }
    server.send(200, "application/json", statusJson());
  });

  server.on("/off", []() { allOff(); server.send(200, "application/json", statusJson()); });

  server.on("/", []() {
    String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<title>zenith</title><style>body{font:16px system-ui;margin:2rem;max-width:34rem}"
                 "button{font:inherit;padding:.6rem 1rem;margin:.2rem}pre{background:#eee;padding:1rem}</style>"
                 "<h1>zenith bench</h1><div>");
    for (size_t i = 0; i < N_LOADS; i++) {
      h += "<button onclick=\"fetch('/set?load=" + String(loads[i].name) + "&on=1').then(r)\">"
           + String(loads[i].name) + " ON</button>";
      h += "<button onclick=\"fetch('/set?load=" + String(loads[i].name) + "&on=0').then(r)\">off</button><br>";
    }
    h += F("</div><button onclick=\"fetch('/off').then(r)\">ALL OFF</button><pre id=s></pre>"
           "<script>function r(){}setInterval(async()=>{"
           "s.textContent=JSON.stringify(await (await fetch('/status')).json(),null,1)},500)</script>");
    server.send(200, "text/html", h);
  });

  server.begin();
}

void setup() {
  // Loads off FIRST, before anything can take time. Cover relay is active-LOW,
  // so its pin must be driven HIGH immediately or the relay clicks in on boot.
  for (size_t i = 0; i < N_LOADS; i++) {
    pinMode(loads[i].pin, OUTPUT);
    loads[i].on = false;
    applyLoad(loads[i]);
  }

  Serial.begin(115200);
  delay(200);

  pinMode(PIN_FLOAT, INPUT_PULLUP);
  pinMode(PIN_COVER_LIMIT, INPUT_PULLUP);
  pinMode(PIN_FLOW, INPUT);            // GPIO35 input-only: needs an external
                                       // 10k pull-up to 3V3 on the signal line
  analogSetPinAttenuation(PIN_NTC1, ADC_11db);
  analogSetPinAttenuation(PIN_GREEN, ADC_11db);

  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onFlowPulse, FALLING);

  Serial.println(F("\nzenith bench firmware up. All loads OFF."));
  startWifi();
  if (wifiUp) startWeb();
  printHelp();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    switch (c) {
      case 'h': setLoad(0, !loads[0].on); break;
      case 'p': setLoad(1, !loads[1].on); break;
      case 'g': setLoad(2, !loads[2].on); break;
      case 'c': setLoad(3, !loads[3].on); break;
      case '0': allOff(); Serial.println(F("ALL OFF")); break;
      case 's': telemetry(); break;
      case 'z': flowPulses = 0; lastFlowSnapshot = 0; Serial.println(F("flow zeroed")); break;
      case 'U': unsafeAllowDryPump = true; Serial.println(F("dry-pump override ARMED")); break;
      case 'W': saveWifi(Serial.readStringUntil('\n')); break;
      case 'i': Serial.printf("wifi=%d ota=%d ip=%s\n", wifiUp, otaUp,
                              wifiUp ? WiFi.localIP().toString().c_str() : "-"); break;
      case '?': printHelp(); break;
      default: break;
    }
  }

  if (otaUp)  ArduinoOTA.handle();
  if (wifiUp) server.handleClient();

  uint32_t now = millis();

  // Command watchdog: nothing stays on unattended.
  for (size_t i = 0; i < N_LOADS; i++) {
    if (loads[i].on && (now - loads[i].lastCmdMs) > CMD_WATCHDOG_MS) {
      loads[i].on = false;
      applyLoad(loads[i]);
      Serial.printf("watchdog: %s forced off\n", loads[i].name);
    }
  }

  if (now - lastTelemetryMs >= TELEMETRY_MS) {
    lastTelemetryMs = now;
    telemetry();
  }
}
