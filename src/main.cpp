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
#include <Update.h>

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

// Build stamp. Its whole job is to make an OTA push visibly land: read it back
// from /status and a firmware that arrived over the air is proven, not assumed.
static const char FW_BUILD[] = __DATE__ " " __TIME__;

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
static String            lineBuf;
static uint32_t          lastWifiTryMs = 0;
static const uint32_t    WIFI_RETRY_MS = 30000;

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
  Serial.println(F("  S       : scan for wifi networks (numbered list)"));
  Serial.println(F("  C..     : join scanned network, C<number>/<password>"));
  Serial.println(F("  W..     : join by name, W<ssid>/<password>"));
  Serial.println(F("  L       : list stored networks"));
  Serial.println(F("  i       : wifi/OTA/IP info"));
  Serial.println(F("  ?       : this help"));
  Serial.println(F("Every command needs Enter. Loads auto-off after 15s (watchdog).\n"));
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
// The machine moves between networks, and once it is closed up the USB port is
// unreachable. So WiFi config must never require a cable:
//   * up to MAX_NETS networks are remembered and tried in turn at boot;
//   * if none come up, the ESP raises its own access point and serves a setup
//     page listing the networks it can see, so a new one can be joined from
//     any laptop without opening the machine.

static const int MAX_NETS = 3;

static String scanSsids[16];
static int    scanCount = 0;
static bool   apMode    = false;

static const char *AP_SSID = "zenith-setup";
static const char *AP_PASS = "zenith1234";

static void wifiScan() {
  Serial.println(F("scanning for networks..."));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  scanCount = (n > 16) ? 16 : (n < 0 ? 0 : n);
  for (int i = 0; i < scanCount; i++) {
    scanSsids[i] = WiFi.SSID(i);
    Serial.printf("  [%d] %-32s %4d dBm%s\n", i, scanSsids[i].c_str(),
                  WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "  (open)" : "");
  }
  if (scanCount == 0) Serial.println(F("  (none found)"));
}

static void addNetwork(const String &ssid, const String &pass) {
  prefs.begin("zenith", false);
  // Read existing, drop any entry with the same name, push the new one to front.
  String ss[MAX_NETS], pp[MAX_NETS];
  int count = prefs.getInt("n", 0);
  int k = 0;
  for (int i = 0; i < count && k < MAX_NETS - 1; i++) {
    String s2 = prefs.getString(("ssid" + String(i)).c_str(), "");
    if (s2.isEmpty() || s2 == ssid) continue;
    ss[k] = s2;
    pp[k] = prefs.getString(("pass" + String(i)).c_str(), "");
    k++;
  }
  prefs.putString("ssid0", ssid);
  prefs.putString("pass0", pass);
  for (int i = 0; i < k; i++) {
    prefs.putString(("ssid" + String(i + 1)).c_str(), ss[i]);
    prefs.putString(("pass" + String(i + 1)).c_str(), pp[i]);
  }
  prefs.putInt("n", k + 1);
  prefs.end();
  Serial.printf("saved network \"%s\" (%d stored)\n", ssid.c_str(), k + 1);
}

static void listNetworks() {
  prefs.begin("zenith", true);
  int count = prefs.getInt("n", 0);
  Serial.printf("stored networks: %d\n", count);
  for (int i = 0; i < count; i++)
    Serial.printf("  %d. %s\n", i, prefs.getString(("ssid" + String(i)).c_str(), "").c_str());
  prefs.end();
}

static bool tryConnect(const String &ssid, const String &pass, uint32_t timeoutMs) {
  Serial.printf("WiFi: trying \"%s\" ...", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("zenith");
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " ok" : " failed");
  return WiFi.status() == WL_CONNECTED;
}

static void startAP() {
  apMode = true;
  wifiScan();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("\nAP MODE. Join wifi \"%s\" (password %s)\n", AP_SSID, AP_PASS);
  Serial.print(F("Then open  http://"));
  Serial.print(WiFi.softAPIP());
  Serial.println(F("/  to pick a network.\n"));
}

static void startOta() {
  ArduinoOTA.setHostname("zenith");
  ArduinoOTA.onStart([]() { allOff(); Serial.println(F("OTA: start, all loads off")); });
  ArduinoOTA.onEnd([]()   { Serial.println(F("OTA: done, rebooting")); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA: error %u\n", e); });
  ArduinoOTA.begin();
  otaUp = true;
  Serial.println(F("OTA: armed"));
}

static void startWifi() {
  prefs.begin("zenith", true);
  int count = prefs.getInt("n", 0);
  String ss[MAX_NETS], pp[MAX_NETS];
  for (int i = 0; i < count && i < MAX_NETS; i++) {
    ss[i] = prefs.getString(("ssid" + String(i)).c_str(), "");
    pp[i] = prefs.getString(("pass" + String(i)).c_str(), "");
  }
  prefs.end();

  for (int i = 0; i < count && i < MAX_NETS; i++) {
    if (ss[i].isEmpty()) continue;
    if (tryConnect(ss[i], pp[i], 12000)) {
      wifiUp = true;
      Serial.print(F("WiFi: connected, IP = "));
      Serial.println(WiFi.localIP());
      if (MDNS.begin("zenith")) Serial.println(F("mDNS: http://zenith.local/"));
      startOta();
      return;
    }
  }

  Serial.println(F("WiFi: no stored network reachable."));
  startAP();
}

// Re-join without a cable. A single attempt at boot was not enough: the reboot
// at the end of an OTA came back with the radio unhappy, and one failure meant
// AP mode until someone power-cycled it. A router reboot would have done the
// same. So keep trying, and climb back out of AP mode on our own.
//
// Only ever attempted with every load off. Reconnecting blocks for seconds, and
// nothing that can switch mains should sit unattended inside a blocking call.
static void wifiTick() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) {
      wifiUp = true;
      Serial.print(F("WiFi: back up, IP = "));
      Serial.println(WiFi.localIP());
    }
    return;
  }
  if (wifiUp) {
    wifiUp = false;
    Serial.println(F("WiFi: link lost"));
  }

  for (size_t i = 0; i < N_LOADS; i++)
    if (loads[i].on) return;

  if (millis() - lastWifiTryMs < WIFI_RETRY_MS) return;
  lastWifiTryMs = millis();

  prefs.begin("zenith", true);
  int count = prefs.getInt("n", 0);
  for (int i = 0; i < count && i < MAX_NETS; i++) {
    String ss = prefs.getString(("ssid" + String(i)).c_str(), "");
    String pp = prefs.getString(("pass" + String(i)).c_str(), "");
    if (ss.isEmpty()) continue;
    if (tryConnect(ss, pp, 10000)) {
      prefs.end();
      wifiUp = true;
      apMode = false;
      Serial.print(F("WiFi: reconnected, IP = "));
      Serial.println(WiFi.localIP());
      MDNS.begin("zenith");
      if (!otaUp) startOta();
      return;
    }
  }
  prefs.end();
}

static String statusJson() {
  uint32_t ntcMv = 0, greenMv = 0;
  float ntcOhms   = dividerOhms(PIN_NTC1, ntcMv);
  float greenOhms = dividerOhms(PIN_GREEN, greenMv);
  String j = "{";
  j += "\"fw\":\"" + String(FW_BUILD) + "\",";
  j += "\"uptime_s\":" + String(millis() / 1000) + ",";
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

// Every route is registered once, unconditionally. Registering them per-mode
// meant a board that booted into AP mode and only later found the network kept
// serving the setup page forever -- including no /update, so the one bug that
// stranded it was also the one bug OTA could not reach past.
static void startWeb() {
  static bool started = false;
  if (started) return;
  started = true;

  //   curl -F firmware=@firmware.bin http://zenith.local/update
  server.on("/update", HTTP_POST,
    []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", Update.hasError() ? "FAIL\n" : "OK, rebooting\n");
      delay(400);
      WiFi.disconnect(true);
      delay(200);
      ESP.restart();
    },
    []() {
      HTTPUpload &up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        allOff();                       // never swap firmware with a load live
        Serial.printf("HTTP OTA: receiving %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("HTTP OTA: %u bytes, rebooting\n", up.totalSize);
        else Update.printError(Serial);
      }
    });

  server.on("/status", []() { server.send(200, "application/json", statusJson()); });

  server.on("/set", []() {
    String name = server.arg("load");
    bool on = server.arg("on") == "1";
    for (size_t i = 0; i < N_LOADS; i++)
      if (name == loads[i].name) { setLoad(i, on); break; }
    server.send(200, "application/json", statusJson());
  });

  server.on("/off", []() { allOff(); server.send(200, "application/json", statusJson()); });

  // Reachable while connected too, so the next network can be stored in advance
  // -- the machine moves, and by then the USB port is behind the joints.
  server.on("/wifi", []() {
    if (scanCount == 0) wifiScan();
    String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<title>zenith wifi</title><style>body{font:16px system-ui;margin:2rem;max-width:28rem}"
                 "select,input,button{font:inherit;padding:.5rem;width:100%;margin:.3rem 0}</style>"
                 "<h1>zenith wifi</h1><p>Up to 3 networks are remembered and tried at boot.</p>"
                 "<form action=/save method=get><select name=ssid>");
    for (int i = 0; i < scanCount; i++) h += "<option>" + scanSsids[i] + "</option>";
    h += F("</select><input name=pass type=password placeholder='wifi password'>"
           "<button>save &amp; reboot</button></form>");
    server.send(200, "text/html", h);
  });

  server.on("/save", []() {
    addNetwork(server.arg("ssid"), server.arg("pass"));
    server.send(200, "text/html", F("<h1>saved</h1><p>rebooting.</p>"));
    delay(400);
    ESP.restart();
  });

  server.on("/", []() {
    if (apMode) { server.sendHeader("Location", "/wifi"); server.send(302, "text/plain", ""); return; }
    String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<title>zenith</title><style>body{font:16px system-ui;margin:2rem;max-width:34rem}"
                 "button{font:inherit;padding:.6rem 1rem;margin:.2rem}pre{background:#eee;padding:1rem;"
                 "overflow-x:auto}</style><h1>zenith bench</h1><div>");
    for (size_t i = 0; i < N_LOADS; i++) {
      h += "<button onclick=\"fetch('/set?load=" + String(loads[i].name) + "&on=1')\">"
           + String(loads[i].name) + " ON</button>";
      h += "<button onclick=\"fetch('/set?load=" + String(loads[i].name) + "&on=0')\">off</button><br>";
    }
    h += F("</div><button onclick=\"fetch('/off')\">ALL OFF</button>"
           " <a href=/wifi>wifi</a><pre id=s></pre>"
           "<script>setInterval(async()=>{s.textContent="
           "JSON.stringify(await (await fetch('/status')).json(),null,1)},500)</script>");
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

  Serial.printf("\nzenith bench firmware up (build %s). All loads OFF.\n", FW_BUILD);
  startWifi();
  startWeb();
  printHelp();
}

void loop() {
  // Line-buffered, so a command is only acted on once the whole line has
  // arrived. Reading the argument inline instead raced the operator's typing:
  // the stream timeout expired mid-password and stored a truncated one.
  // Requiring Enter also means a stray keystroke can no longer switch a load.
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (lineBuf.length() < 160) lineBuf += c;
      continue;
    }

    String line = lineBuf;
    lineBuf = "";
    line.trim();
    if (line.isEmpty()) continue;

    char   cmd = line.charAt(0);
    String arg = line.substring(1);
    arg.trim();

    switch (cmd) {
      case 'h': setLoad(0, !loads[0].on); break;
      case 'p': setLoad(1, !loads[1].on); break;
      case 'g': setLoad(2, !loads[2].on); break;
      case 'c': setLoad(3, !loads[3].on); break;
      case '0': allOff(); Serial.println(F("ALL OFF")); break;
      case 's': telemetry(); break;
      case 'z': flowPulses = 0; lastFlowSnapshot = 0; Serial.println(F("flow zeroed")); break;
      case 'U': unsafeAllowDryPump = true; Serial.println(F("dry-pump override ARMED")); break;
      case 'S': wifiScan(); break;
      case 'L': listNetworks(); break;

      case 'C': {  // C<index>/<password>
        int sl = arg.indexOf('/');
        int idx = (sl < 0) ? arg.toInt() : arg.substring(0, sl).toInt();
        String pw = (sl < 0) ? "" : arg.substring(sl + 1);
        if (idx < 0 || idx >= scanCount) { Serial.println(F("bad index - run S first")); break; }
        Serial.printf("joining \"%s\" with a %d-character password\n",
                      scanSsids[idx].c_str(), pw.length());
        addNetwork(scanSsids[idx], pw);
        Serial.println(F("rebooting..."));
        delay(300); ESP.restart();
        break;
      }

      case 'W': {  // W<ssid>/<password>
        int sl = arg.indexOf('/');
        if (sl < 1) { Serial.println(F("usage: W<ssid>/<password>")); break; }
        String ss = arg.substring(0, sl), pw = arg.substring(sl + 1);
        // Echo the length, never the password: catches a truncated paste
        // without putting the secret in the scrollback.
        Serial.printf("joining \"%s\" with a %d-character password\n",
                      ss.c_str(), pw.length());
        addNetwork(ss, pw);
        Serial.println(F("rebooting..."));
        delay(300); ESP.restart();
        break;
      }

      case 'i':
        Serial.printf("wifi=%d ota=%d ap=%d ip=%s\n", wifiUp, otaUp, apMode,
                      wifiUp   ? WiFi.localIP().toString().c_str()
                      : apMode ? WiFi.softAPIP().toString().c_str() : "-");
        break;

      case '?': printHelp(); break;
      default: Serial.printf("unknown command '%c' - try ?\n", cmd); break;
    }
  }

  wifiTick();
  if (otaUp)  ArduinoOTA.handle();
  server.handleClient();

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
