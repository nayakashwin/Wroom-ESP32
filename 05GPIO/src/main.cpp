#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

static const uint32_t BAUD      = 115200;
static const size_t   CMDBUF_MAX  = 96;
static const uint8_t  LEDC_MAX  = 16;

/* ------------------------------------------------------------------ *
 * NodeMCU ESP-32S v1.1 pin capability table (ESP-WROOM-32 module)
 *
 * broken out: 0 1 2 3 4 5 12 13 14 15 16 17 18 19 21 22 23
 *             25 26 27 32 33 34 35 36 39   (+ EN, 3V3, GND, VIN)
 * never touched: 6..11 (SPI flash), 37/38 (not bonded), EN (reset)
 * ------------------------------------------------------------------ */
enum : uint16_t {
  F_OUT   = 1u << 0,   // digital output capable (all but 34 35 36 39)
  F_PULL  = 1u << 1,   // internal pull-up / pull-down (all but 34 35 36 39)
  F_STRAP = 1u << 2,   // boot strapping pin
  F_UART0 = 1u << 3,   // USB serial console (TX0/RX0)
  F_UART2 = 1u << 4,   // default UART2 (RX2/TX2)
  F_TOUCH = 1u << 5,   // capacitive touch channel
  F_DAC   = 1u << 6,   // 8-bit DAC output
};

struct PinDef {
  uint8_t  gpio;
  uint16_t flags;
};

static const PinDef PINS[] = {
  { 0, F_OUT | F_PULL | F_STRAP | F_TOUCH },
  { 1, F_OUT | F_PULL | F_UART0 },
  { 2, F_OUT | F_PULL | F_STRAP | F_TOUCH },
  { 3, F_OUT | F_PULL | F_UART0 },
  { 4, F_OUT | F_PULL | F_TOUCH },
  { 5, F_OUT | F_PULL | F_STRAP },
  {12, F_OUT | F_PULL | F_STRAP | F_TOUCH },
  {13, F_OUT | F_PULL | F_TOUCH },
  {14, F_OUT | F_PULL | F_TOUCH },
  {15, F_OUT | F_PULL | F_STRAP | F_TOUCH },
  {16, F_OUT | F_PULL | F_UART2 },
  {17, F_OUT | F_PULL | F_UART2 },
  {18, F_OUT | F_PULL },
  {19, F_OUT | F_PULL },
  {21, F_OUT | F_PULL },
  {22, F_OUT | F_PULL },
  {23, F_OUT | F_PULL },
  {25, F_OUT | F_PULL | F_TOUCH | F_DAC },
  {26, F_OUT | F_PULL | F_TOUCH | F_DAC },
  {27, F_OUT | F_PULL | F_TOUCH },
  {32, F_OUT | F_PULL | F_TOUCH },
  {33, F_OUT | F_PULL | F_TOUCH },
  {34, 0 },   // input only: no output driver, no pull-ups
  {35, 0 },
  {36, 0 },   // SVP
  {39, 0 },   // SVN
};
static const size_t N_PINS = sizeof(PINS) / sizeof(PINS[0]);

static const uint8_t INPUT_ONLY[] = { 34, 35, 36, 39 };

/* per-GPIO runtime state, indexed by gpio number */
struct State {
  int8_t   mode;      // -1 untouched, else INPUT / OUTPUT / INPUT_PULLUP / INPUT_PULLDOWN
  bool     pwm;       // LEDC attached
  int8_t   ch;        // LEDC channel, -1 = none
  bool     blink;
  uint32_t onMs;
  uint32_t offMs;
  uint32_t due;
  uint8_t  level;     // last driven level on an output
};
static State st[40];
static bool  ledcUsed[LEDC_MAX];

static char     line[CMDBUF_MAX];
static size_t   lineLen = 0;

/* ---------------------------------------------------------------- helpers */

static int pinIndex(uint8_t g) {
  for (size_t i = 0; i < N_PINS; i++) if (PINS[i].gpio == g) return (int)i;
  return -1;
}

static uint16_t pinFlags(uint8_t g) {
  int i = pinIndex(g);
  return i < 0 ? 0 : PINS[i].flags;
}

static bool isInputOnly(uint8_t g) {
  for (size_t i = 0; i < sizeof(INPUT_ONLY); i++) if (INPUT_ONLY[i] == g) return true;
  return false;
}

static bool ciEq(const char* a, const char* b) {
  while (*a && *b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

static const char* modeName(int8_t m) {
  if (m == OUTPUT)              return "out";
  if (m == INPUT)               return "in";
  if (m == INPUT_PULLUP)        return "in-pu";
  if (m == INPUT_PULLDOWN)      return "in-pd";
  return "-";
}

/* "A1.6" / "-" — ADC unit+channel straight from the core */
static void adcName(uint8_t g, char* out, size_t n) {
  int8_t ch = digitalPinToAnalogChannel(g);
  if (ch < 0) { snprintf(out, n, "-"); return; }
  snprintf(out, n, "A%u.%u", (unsigned)(ch / 10 + 1), (unsigned)(ch % 10));
}

static void classStr(uint8_t g, char* out, size_t n) {
  uint16_t f = pinFlags(g);
  out[0] = 0;
  auto add = [&](const char* s) {
    size_t left = n - strlen(out);
    if (left < 2) return;
    if (out[0]) { strncat(out, ",", left - 1); left = n - strlen(out); }
    strncat(out, s, left - 1);
  };
  if (!(f & F_OUT)) add("in-only");
  if (f & F_STRAP) add("strap");
  if (f & F_UART0) add("uart0");
  if (f & F_UART2) add("uart2");
  if (f & F_DAC)   add("dac");
  if (f & F_TOUCH) add("touch");
  if (!out[0])     add("gpio");
}

/* print the caveats that matter before a pin is reconfigured */
static void warnPin(uint8_t g) {
  uint16_t f = pinFlags(g);
  if (f & F_STRAP) {
    if (g == 0)  Serial.println("note: GPIO0 strap - low at reset = download mode (BOOT button)");
    if (g == 2)  Serial.println("note: GPIO2 strap - must be low/floating at boot");
    if (g == 5)  Serial.println("note: GPIO5 strap - SDIO timing, expect a pulse at boot");
    if (g == 12) Serial.println("note: GPIO12 strap (MTDI) - HIGH at reset breaks boot on 3.3V flash");
    if (g == 15) Serial.println("note: GPIO15 strap - affects boot log / SDIO timing");
  }
  if (f & F_UART0) Serial.println("note: GPIO1/3 are UART0 - driving them garbles this console");
  if (f & F_UART2) Serial.println("note: GPIO16/17 are the default UART2 pins");
  if (f & F_DAC)   Serial.println("note: GPIO25/26 have a DAC (dacWrite) as well as PWM");
}

static int allocChannel() {
  for (uint8_t i = 0; i < LEDC_MAX; i++) {
    if (!ledcUsed[i]) { ledcUsed[i] = true; return i; }
  }
  return -1;
}

static void freeChannel(int8_t ch) {
  if (ch >= 0 && ch < LEDC_MAX) ledcUsed[ch] = false;
}

static void detachPwm(uint8_t g, bool quiet) {
  if (!st[g].pwm) return;
  ledcDetachPin(g);
  freeChannel(st[g].ch);
  st[g].pwm = false;
  st[g].ch  = -1;
  if (!quiet) Serial.printf("ok: gpio %u pwm off\n", g);
}

static void stopBlink(uint8_t g) {
  if (!st[g].blink) return;
  st[g].blink = false;
}

/* make sure the pin is an output we are allowed to drive */
static bool ensureOutput(uint8_t g) {
  if (st[g].mode == OUTPUT) return true;
  if (isInputOnly(g)) {
    Serial.printf("ERR: gpio %u is input-only (34/35/36/39 have no output driver)\n", g);
    return false;
  }
  if (st[g].mode != -1) {
    Serial.printf("ERR: gpio %u is %s - use: mode %u out\n", g, modeName(st[g].mode), g);
    return false;
  }
  detachPwm(g, true);
  stopBlink(g);
  pinMode(g, OUTPUT);
  st[g].mode  = OUTPUT;
  st[g].level = 0;
  Serial.printf("auto: gpio %u set to output (low)\n", g);
  warnPin(g);
  return true;
}

/* ------------------------------------------------------------- commands */

static void cmdHelp() {
  Serial.println(
    "\n"
    "--- 05GPIO commands (one per line, 115200 baud) ---\n"
    "help                  this text\n"
    "pins                  table of all 26 GPIOs: class / mode / level / adc\n"
    "mode <p> <m>          m = in | out | pu | pd   (pu/pd = pull-up/pull-down)\n"
    "w <p> <0|1>           digital write (auto mode out if the pin is untouched)\n"
    "r <p>                 digital read\n"
    "a <p>                 analog read: raw 0-4095 + millivolts\n"
    "pwm <p> <Hz> <d>      LEDC pwm, 8-bit duty d = 0..255\n"
    "blink <p> <on> <off>  non-blocking blink, milliseconds\n"
    "tog <p>               toggle the driven level\n"
    "stop <p>              detach pwm / stop blink\n"
    "all <0|1>             write every pin currently in mode out\n"
    "stat                  heap, uptime, used pwm channels\n"
    "\n"
    "<p> is a bare GPIO number, e.g.  mode 25 out  /  w 25 1  /  a 36\n"
    "excluded: 6-11 (SPI flash), 37/38 (not bonded), EN (reset)\n");
}

static void cmdPins() {
  Serial.println("pin class                mode   lvl  adc");
  for (size_t i = 0; i < N_PINS; i++) {
    uint8_t g = PINS[i].gpio;
    char cls[40], adc[8];
    classStr(g, cls, sizeof(cls));
    adcName(g, adc, sizeof(adc));
    const char* lvl = "-";
    if (st[g].mode != -1) lvl = digitalRead(g) ? "1" : "0";
    Serial.printf("%3u %-21s %-6s %-4s %s\n", g, cls, modeName(st[g].mode), lvl, adc);
  }
  Serial.println("lvl is live for configured pins only; '-' = untouched (hi-z)");
}

static void cmdMode(uint8_t g, const char* what) {
  if (!what) { Serial.println("ERR: usage: mode <p> <in|out|pu|pd>"); return; }
  int8_t want;
  if (ciEq(what, "in") || ciEq(what, "input"))                 want = INPUT;
  else if (ciEq(what, "out") || ciEq(what, "output"))          want = OUTPUT;
  else if (ciEq(what, "pu") || ciEq(what, "pullup"))           want = INPUT_PULLUP;
  else if (ciEq(what, "pd") || ciEq(what, "pulldown"))         want = INPUT_PULLDOWN;
  else { Serial.printf("ERR: unknown mode '%s'\n", what); return; }

  if (want != INPUT && isInputOnly(g)) {
    Serial.printf("ERR: gpio %u is input-only (34/35/36/39)\n", g);
    return;
  }
  if (want != INPUT && (pinFlags(g) & F_PULL) == 0) {
    Serial.printf("ERR: gpio %u has no internal pull resistors\n", g);
    return;
  }

  detachPwm(g, true);
  stopBlink(g);
  pinMode(g, want);
  if (want == OUTPUT) { digitalWrite(g, LOW); st[g].level = 0; }
  st[g].mode = want;
  Serial.printf("ok: gpio %u mode = %s\n", (unsigned)g, modeName(want));
  if (want == OUTPUT) warnPin(g);
}

static void cmdWrite(uint8_t g, const char* v) {
  if (!v) { Serial.println("ERR: usage: w <p> <0|1>"); return; }
  uint8_t lvl = (v[0] == '1' || ciEq(v, "high") || ciEq(v, "on")) ? 1
              : (v[0] == '0' || ciEq(v, "low")  || ciEq(v, "off")) ? 0 : 2;
  if (lvl == 2) { Serial.println("ERR: level must be 0 or 1"); return; }
  if (!ensureOutput(g)) return;
  if (st[g].blink) { stopBlink(g); Serial.printf("note: blink stopped on gpio %u\n", g); }
  if (st[g].pwm)   detachPwm(g, false);
  digitalWrite(g, lvl);
  st[g].level = lvl;
  Serial.printf("ok: gpio %u = %u\n", (unsigned)g, (unsigned)lvl);
}

static void cmdRead(uint8_t g) {
  int v = digitalRead(g);
  Serial.printf("gpio %u mode=%s value=%d\n", g, modeName(st[g].mode), v);
}

static void cmdAnalog(uint8_t g) {
  int8_t ch = digitalPinToAnalogChannel(g);
  if (ch < 0) { Serial.printf("ERR: gpio %u has no ADC channel\n", g); return; }
  uint16_t raw = analogRead(g);
  uint32_t mv  = analogReadMilliVolts(g);
  Serial.printf("gpio %u A%u.%u raw=%u v=%lu mV\n",
                g, (unsigned)(ch / 10 + 1), (unsigned)(ch % 10),
                raw, (unsigned long)mv);
}

static void cmdPwm(uint8_t g, const char* sfreq, const char* sduty) {
  if (!sfreq || !sduty) { Serial.println("ERR: usage: pwm <p> <Hz> <0-255>"); return; }
  if (isInputOnly(g)) { Serial.printf("ERR: gpio %u is input-only\n", g); return; }
  uint32_t freq = (uint32_t)strtoul(sfreq, NULL, 10);
  long duty = strtol(sduty, NULL, 10);
  if (freq < 1 || freq > 40000000UL) { Serial.println("ERR: freq must be 1..40000000 Hz"); return; }
  if (duty < 0 || duty > 255) { Serial.println("ERR: duty must be 0..255"); return; }

  if (st[g].mode != -1 && st[g].mode != OUTPUT) {
    Serial.printf("ERR: gpio %u is %s - use: mode %u out\n", g, modeName(st[g].mode), g);
    return;
  }
  if (st[g].mode == -1) {
    pinMode(g, OUTPUT);
    st[g].mode = OUTPUT;
    st[g].level = 0;
    Serial.printf("auto: gpio %u set to output\n", g);
    warnPin(g);
  }
  stopBlink(g);
  if (!st[g].pwm) {
    int ch = allocChannel();
    if (ch < 0) { Serial.println("ERR: no free LEDC channel (16 in use)"); return; }
    st[g].ch  = (int8_t)ch;
    st[g].pwm = true;
  }
  ledcSetup((uint8_t)st[g].ch, freq, 8);
  ledcAttachPin(g, (uint8_t)st[g].ch);
  ledcWrite((uint8_t)st[g].ch, (uint32_t)duty);
  Serial.printf("ok: gpio %u pwm ch=%d freq=%lu Hz duty=%ld/255 (%lu%%)\n",
                g, st[g].ch, (unsigned long)freq, duty,
                (unsigned long)(duty * 100UL / 255UL));
}

static void cmdBlink(uint8_t g, const char* son, const char* soff) {
  if (!son || !soff) { Serial.println("ERR: usage: blink <p> <on_ms> <off_ms>"); return; }
  long on = strtol(son, NULL, 10), off = strtol(soff, NULL, 10);
  if (on < 1 || off < 1 || on > 3600000L || off > 3600000L) {
    Serial.println("ERR: times must be 1..3600000 ms"); return;
  }
  if (!ensureOutput(g)) return;
  detachPwm(g, true);
  st[g].blink = true;
  st[g].onMs  = (uint32_t)on;
  st[g].offMs = (uint32_t)off;
  st[g].level = 1;
  digitalWrite(g, HIGH);
  st[g].due = millis() + (uint32_t)on;
  Serial.printf("ok: gpio %u blink %ld ms on / %ld ms off\n", g, on, off);
}

static void cmdToggle(uint8_t g) {
  if (!ensureOutput(g)) return;
  if (st[g].blink) { stopBlink(g); Serial.printf("note: blink stopped on gpio %u\n", g); }
  detachPwm(g, true);
  st[g].level = st[g].level ? 0 : 1;
  digitalWrite(g, st[g].level);
  Serial.printf("ok: gpio %u = %u\n", g, st[g].level);
}

static void cmdStop(uint8_t g) {
  bool had = st[g].pwm || st[g].blink;
  detachPwm(g, true);
  stopBlink(g);
  if (!had) Serial.printf("ok: gpio %u had nothing running\n", g);
  else      Serial.printf("ok: gpio %u stopped (mode stays %s)\n", g, modeName(st[g].mode));
}

static void cmdAll(const char* v) {
  if (!v) { Serial.println("ERR: usage: all <0|1>"); return; }
  uint8_t lvl = (v[0] == '1') ? 1 : (v[0] == '0') ? 0 : 2;
  if (lvl == 2) { Serial.println("ERR: level must be 0 or 1"); return; }
  int n = 0, stopped = 0, detached = 0;
  for (size_t i = 0; i < N_PINS; i++) {
    uint8_t g = PINS[i].gpio;
    if (st[g].mode != OUTPUT) continue;
    if (st[g].blink) { stopBlink(g); stopped++; }
    if (st[g].pwm)   { detachPwm(g, true); detached++; }
    digitalWrite(g, lvl);
    st[g].level = lvl;
    n++;
  }
  if (n == 0) Serial.println("ERR: no pin is in mode out yet (use: mode <p> out)");
  else        Serial.printf("ok: all %d output(s) <- %u%s%s\n", n, (unsigned)lvl,
                            stopped ? " (blink stopped)" : "",
                            detached ? " (pwm off)" : "");
}

static void cmdStat() {
  int outs = 0, pwms = 0, blinks = 0, cfgd = 0;
  for (size_t i = 0; i < N_PINS; i++) {
    State& s = st[PINS[i].gpio];
    if (s.mode != -1) cfgd++;
    if (s.mode == OUTPUT) outs++;
    if (s.pwm)   pwms++;
    if (s.blink) blinks++;
  }
  Serial.printf("stat: gpio configured=%d outputs=%d pwm=%d blink=%d | heap free=%u min=%u | uptime=%lu s\n",
                cfgd, outs, pwms, blinks,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                (unsigned long)(millis() / 1000));
}

/* ------------------------------------------------------------- dispatch */

static void handleLine(char* buf) {
  char* cmd = strtok(buf, " \t");
  if (!cmd) return;

  /* numeric first argument, where needed */
  auto takePin = [&](uint8_t* g) -> char* {
    char* a = strtok(NULL, " \t");
    if (!a) { Serial.println("ERR: missing <p>"); return NULL; }
    char* end = NULL;
    long v = strtol(a, &end, 10);
    if (end == a || *end || v < 0 || v > 39) {
      Serial.printf("ERR: '%s' is not a GPIO number 0..39\n", a);
      return NULL;
    }
    if (pinIndex((uint8_t)v) < 0) {
      Serial.printf("ERR: gpio %ld is not available on this board (6-11 = flash, 37/38 not bonded)\n", v);
      return NULL;
    }
    *g = (uint8_t)v;
    return a;
  };

  if (ciEq(cmd, "help") || ciEq(cmd, "?") || ciEq(cmd, "h")) { cmdHelp(); return; }
  if (ciEq(cmd, "pins") || ciEq(cmd, "list"))                 { cmdPins(); return; }
  if (ciEq(cmd, "stat") || ciEq(cmd, "status"))                { cmdStat(); return; }

  if (ciEq(cmd, "mode")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdMode(g, strtok(NULL, " \t"));
    return;
  }
  if (ciEq(cmd, "w") || ciEq(cmd, "write") || ciEq(cmd, "digitalwrite")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdWrite(g, strtok(NULL, " \t"));
    return;
  }
  if (ciEq(cmd, "r") || ciEq(cmd, "read") || ciEq(cmd, "digitalread")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdRead(g);
    return;
  }
  if (ciEq(cmd, "a") || ciEq(cmd, "adc") || ciEq(cmd, "analog")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdAnalog(g);
    return;
  }
  if (ciEq(cmd, "pwm")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdPwm(g, strtok(NULL, " \t"), strtok(NULL, " \t"));
    return;
  }
  if (ciEq(cmd, "blink")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdBlink(g, strtok(NULL, " \t"), strtok(NULL, " \t"));
    return;
  }
  if (ciEq(cmd, "tog") || ciEq(cmd, "toggle")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdToggle(g);
    return;
  }
  if (ciEq(cmd, "stop")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdStop(g);
    return;
  }
  if (ciEq(cmd, "all")) {
    cmdAll(strtok(NULL, " \t"));
    return;
  }

  Serial.printf("ERR: unknown command '%s' - type 'help'\n", cmd);
}

/* ---------------------------------------------------------------- setup */

static void serviceBlink() {
  uint32_t now = millis();
  for (size_t i = 0; i < N_PINS; i++) {
    uint8_t g = PINS[i].gpio;
    if (!st[g].blink) continue;
    if ((int32_t)(now - st[g].due) < 0) continue;
    st[g].level = st[g].level ? 0 : 1;
    digitalWrite(g, st[g].level);
    st[g].due = now + (st[g].level ? st[g].onMs : st[g].offMs);
  }
}

static void pollSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) return;
    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLen] = 0;
      lineLen = 0;
      if (line[0]) handleLine(line);
      continue;
    }
    if (lineLen < CMDBUF_MAX - 1) line[lineLen++] = (char)c;
    else { lineLen = 0; Serial.println("ERR: line too long"); }
  }
}

void setup() {
  Serial.begin(BAUD);
  delay(200);
  for (size_t i = 0; i < N_PINS; i++) {
    State& s = st[PINS[i].gpio];
    memset(&s, 0, sizeof(s));
    s.mode = -1;
    s.ch   = -1;
  }
  for (uint8_t i = 0; i < LEDC_MAX; i++) ledcUsed[i] = false;

  Serial.printf("\n05GPIO online: %u controllable gpio, %lu baud\n",
                (unsigned)N_PINS, (unsigned long)BAUD);
  Serial.print("pins:");
  for (size_t i = 0; i < N_PINS; i++) Serial.printf(" %u", PINS[i].gpio);
  Serial.println();
  Serial.println("left untouched (hi-z) until you drive them; type 'help'");
  cmdHelp();
}

void loop() {
  pollSerial();
  serviceBlink();
}
