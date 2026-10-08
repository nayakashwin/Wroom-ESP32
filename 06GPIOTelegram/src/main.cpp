#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

/* ---------------- secrets (in _secrets/secrets.h, never committed) --------
 * Same set as 04TelegramRXTX: WIFI_SSID / WIFI_PASS, BOT_TOKEN and CHAT_ID
 * (numeric id, @channelusername or -100... for a group). CHAT_ID is the only
 * chat allowed to drive the GPIO.
 */
#if !defined(__has_include) || !__has_include("secrets.h")
#error "secrets.h not found - copy _secrets/secrets.example.h to _secrets/secrets.h and fill it in"
#endif
#include "secrets.h"

/* ---------------- telegram transport config ---------------- */
static const uint32_t POLL_TIMEOUT_S = 2;
static const uint32_t POLL_LIMIT = 10;
static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t RETRY_DELAY_MS = 3000;
static const size_t TX_LINE_MAX = 160;
static const uint32_t HEAP_WARN_BYTES = 40000;

/* ======================================================================== *
 *  GPIO core - the 26 pins broken out on the NodeMCU ESP-32S v1.1
 *  (same table as project 05GPIO; flash pins 6-11 / 37 / 38 / EN refused)
 * ======================================================================== */
enum : uint16_t {
  F_OUT   = 1u << 0,
  F_PULL  = 1u << 1,
  F_STRAP = 1u << 2,
  F_UART0 = 1u << 3,
  F_UART2 = 1u << 4,
  F_TOUCH = 1u << 5,
  F_DAC   = 1u << 6,
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
  {34, 0 },   // input only
  {35, 0 },
  {36, 0 },
  {39, 0 },
};
static const size_t N_PINS = sizeof(PINS) / sizeof(PINS[0]);
static const uint8_t INPUT_ONLY[] = { 34, 35, 36, 39 };
static const uint8_t LEDC_MAX = 16;

struct State {
  int8_t   mode;      // -1 untouched
  bool     pwm;
  int8_t   ch;
  bool     blink;
  uint32_t onMs;
  uint32_t offMs;
  uint32_t due;
  uint8_t  level;
};
static State st[40];
static bool ledcUsed[LEDC_MAX];

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

static bool ciEq(const char *a, const char *b) {
  while (*a && *b) {
    char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
    char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

static const char *modeName(int8_t m) {
  if (m == OUTPUT)         return "out";
  if (m == INPUT)          return "in";
  if (m == INPUT_PULLUP)   return "in-pu";
  if (m == INPUT_PULLDOWN) return "in-pd";
  return "-";
}

static void addf(String &s, const char *fmt, ...) {
  char buf[384];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  s += buf;
}

static void adcName(uint8_t g, char *out, size_t n) {
  int8_t ch = digitalPinToAnalogChannel(g);
  if (ch < 0) { snprintf(out, n, "-"); return; }
  snprintf(out, n, "A%u.%u", (unsigned)(ch / 10 + 1), (unsigned)(ch % 10));
}

static void classStr(uint8_t g, char *out, size_t n) {
  uint16_t f = pinFlags(g);
  out[0] = 0;
  auto add = [&](const char *s) {
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

static void warnPin(uint8_t g, String &out) {
  uint16_t f = pinFlags(g);
  if (f & F_STRAP) {
    if (g == 0)  out += F("note: GPIO0 strap - low at reset = download mode (BOOT)\n");
    if (g == 2)  out += F("note: GPIO2 strap - must be low/floating at boot\n");
    if (g == 5)  out += F("note: GPIO5 strap - SDIO timing, pulse at boot\n");
    if (g == 12) out += F("note: GPIO12 strap (MTDI) - HIGH at reset breaks boot\n");
    if (g == 15) out += F("note: GPIO15 strap - boot log / SDIO timing\n");
  }
  if (f & F_UART0) out += F("note: GPIO1/3 are UART0 - driving them garbles serial\n");
  if (f & F_UART2) out += F("note: GPIO16/17 are the default UART2 pins\n");
  if (f & F_DAC)   out += F("note: GPIO25/26 also have a DAC (dacWrite)\n");
}

static int allocChannel() {
  for (uint8_t i = 0; i < LEDC_MAX; i++) {
    if (!ledcUsed[i]) { ledcUsed[i] = true; return i; }
  }
  return -1;
}

static void detachPwm(uint8_t g, String &out) {
  if (!st[g].pwm) return;
  ledcDetachPin(g);
  if (st[g].ch >= 0 && st[g].ch < LEDC_MAX) ledcUsed[st[g].ch] = false;
  st[g].pwm = false;
  st[g].ch  = -1;
  addf(out, "ok: gpio %u pwm off\n", (unsigned)g);
}

static bool ensureOutput(uint8_t g, String &out) {
  if (st[g].mode == OUTPUT) return true;
  if (isInputOnly(g)) {
    addf(out, "ERR: gpio %u is input-only (34/35/36/39)\n", (unsigned)g);
    return false;
  }
  if (st[g].mode != -1) {
    addf(out, "ERR: gpio %u is %s - use: mode %u out\n",
         (unsigned)g, modeName(st[g].mode), (unsigned)g);
    return false;
  }
  st[g].blink = false;
  pinMode(g, OUTPUT);
  st[g].mode  = OUTPUT;
  st[g].level = 0;
  addf(out, "auto: gpio %u set to output (low)\n", (unsigned)g);
  warnPin(g, out);
  return true;
}

/* ======================================================================== *
 *  command executor - fed by Telegram messages AND by the serial console
 * ======================================================================== */
static void cmdHelp(String &out) {
  out += F(
    "06GPIOTelegram - gpio console over telegram\n"
    "/status            table of all 26 gpio (mode / level / adc)\n"
    "/mode <p> <m>      m = in | out | pu | pd\n"
    "/on <p>            drive high\n"
    "/off <p>           drive low\n"
    "/w <p> <0|1>       write level\n"
    "/read <p>          digital read\n"
    "/adc <p>           analog read: raw 0-4095 + mV\n"
    "/pwm <p> <Hz> <d>  ledc pwm, duty 0-255\n"
    "/blink <p> <on> <off>  blink in ms\n"
    "/tog <p>           toggle\n"
    "/stop <p>          stop pwm / blink\n"
    "/all <0|1>         write every pin in mode out\n"
    "/stat              heap / uptime / pwm channels\n"
    "\n<p> = gpio number 0..39\n"
    "pins: 0-5 12-19 21-23 25-27 32-36 39 (6-11 = flash, never touched)");
}

static void cmdPins(String &out) {
  out += F("pin class                mode   lvl  adc\n");
  for (size_t i = 0; i < N_PINS; i++) {
    uint8_t g = PINS[i].gpio;
    char cls[40], adc[8];
    classStr(g, cls, sizeof(cls));
    adcName(g, adc, sizeof(adc));
    const char *lvl = "-";
    if (st[g].mode != -1) lvl = digitalRead(g) ? "1" : "0";
    addf(out, "%3u %-21s %-6s %-4s %s\n", (unsigned)g, cls, modeName(st[g].mode), lvl, adc);
  }
  out += F("lvl live for configured pins; '-' = untouched (hi-z)");
}

static void cmdStat(String &out) {
  int outs = 0, pwms = 0, blinks = 0, cfgd = 0;
  for (size_t i = 0; i < N_PINS; i++) {
    State &s = st[PINS[i].gpio];
    if (s.mode != -1) cfgd++;
    if (s.mode == OUTPUT) outs++;
    if (s.pwm) pwms++;
    if (s.blink) blinks++;
  }
  addf(out, "configured=%d outputs=%d pwm=%d blink=%d\nheap free=%u min=%u maxblk=%u\nuptime=%lu s",
       cfgd, outs, pwms, blinks, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
       (unsigned)ESP.getMaxAllocHeap(), (unsigned long)(millis() / 1000));
}

static void cmdMode(uint8_t g, const char *what, String &out) {
  if (!what) { out += F("ERR: usage: mode <p> <in|out|pu|pd>\n"); return; }
  int8_t want;
  if (ciEq(what, "in") || ciEq(what, "input"))          want = INPUT;
  else if (ciEq(what, "out") || ciEq(what, "output"))   want = OUTPUT;
  else if (ciEq(what, "pu") || ciEq(what, "pullup"))    want = INPUT_PULLUP;
  else if (ciEq(what, "pd") || ciEq(what, "pulldown"))  want = INPUT_PULLDOWN;
  else { addf(out, "ERR: unknown mode '%s'\n", what); return; }

  if (want != INPUT && isInputOnly(g)) {
    addf(out, "ERR: gpio %u is input-only (34/35/36/39)\n", (unsigned)g);
    return;
  }
  if (want != INPUT && (pinFlags(g) & F_PULL) == 0) {
    addf(out, "ERR: gpio %u has no internal pull resistors\n", (unsigned)g);
    return;
  }

  if (st[g].pwm) detachPwm(g, out);
  st[g].blink = false;
  pinMode(g, want);
  if (want == OUTPUT) { digitalWrite(g, LOW); st[g].level = 0; }
  st[g].mode = want;
  addf(out, "ok: gpio %u mode = %s\n", (unsigned)g, modeName(want));
  if (want == OUTPUT) warnPin(g, out);
}

static void cmdWrite(uint8_t g, const char *v, String &out) {
  if (!v) { out += F("ERR: usage: w <p> <0|1>\n"); return; }
  uint8_t lvl = (v[0] == '1' || ciEq(v, "high") || ciEq(v, "on")) ? 1
              : (v[0] == '0' || ciEq(v, "low")  || ciEq(v, "off")) ? 0 : 2;
  if (lvl == 2) { out += F("ERR: level must be 0 or 1\n"); return; }
  if (!ensureOutput(g, out)) return;
  if (st[g].blink) { st[g].blink = false; addf(out, "note: blink stopped on gpio %u\n", (unsigned)g); }
  if (st[g].pwm)   detachPwm(g, out);
  digitalWrite(g, lvl);
  st[g].level = lvl;
  addf(out, "ok: gpio %u = %u\n", (unsigned)g, (unsigned)lvl);
}

static void cmdRead(uint8_t g, String &out) {
  addf(out, "gpio %u mode=%s value=%d", (unsigned)g, modeName(st[g].mode), digitalRead(g));
}

static void cmdAnalog(uint8_t g, String &out) {
  int8_t ch = digitalPinToAnalogChannel(g);
  if (ch < 0) { addf(out, "ERR: gpio %u has no ADC channel\n", (unsigned)g); return; }
  addf(out, "gpio %u A%u.%u raw=%u v=%lu mV", (unsigned)g, (unsigned)(ch / 10 + 1),
       (unsigned)(ch % 10), (unsigned)analogRead(g), (unsigned long)analogReadMilliVolts(g));
}

static void cmdPwm(uint8_t g, const char *sfreq, const char *sduty, String &out) {
  if (!sfreq || !sduty) { out += F("ERR: usage: pwm <p> <Hz> <0-255>\n"); return; }
  if (isInputOnly(g)) { addf(out, "ERR: gpio %u is input-only\n", (unsigned)g); return; }
  uint32_t freq = (uint32_t)strtoul(sfreq, NULL, 10);
  long duty = strtol(sduty, NULL, 10);
  if (freq < 1 || freq > 40000000UL) { out += F("ERR: freq must be 1..40000000 Hz\n"); return; }
  if (duty < 0 || duty > 255) { out += F("ERR: duty must be 0..255\n"); return; }

  if (st[g].mode != -1 && st[g].mode != OUTPUT) {
    addf(out, "ERR: gpio %u is %s - use: mode %u out\n",
         (unsigned)g, modeName(st[g].mode), (unsigned)g);
    return;
  }
  if (st[g].mode == -1) {
    pinMode(g, OUTPUT);
    st[g].mode = OUTPUT;
    st[g].level = 0;
    addf(out, "auto: gpio %u set to output\n", (unsigned)g);
    warnPin(g, out);
  }
  st[g].blink = false;
  if (!st[g].pwm) {
    int ch = allocChannel();
    if (ch < 0) { out += F("ERR: no free LEDC channel (16 in use)\n"); return; }
    st[g].ch  = (int8_t)ch;
    st[g].pwm = true;
  }
  ledcSetup((uint8_t)st[g].ch, freq, 8);
  ledcAttachPin(g, (uint8_t)st[g].ch);
  ledcWrite((uint8_t)st[g].ch, (uint32_t)duty);
  addf(out, "ok: gpio %u pwm ch=%d freq=%lu Hz duty=%ld/255 (%lu%%)", (unsigned)g, st[g].ch,
       (unsigned long)freq, duty, (unsigned long)(duty * 100UL / 255UL));
}

static void cmdBlink(uint8_t g, const char *son, const char *soff, String &out) {
  if (!son || !soff) { out += F("ERR: usage: blink <p> <on_ms> <off_ms>\n"); return; }
  long on = strtol(son, NULL, 10), off = strtol(soff, NULL, 10);
  if (on < 1 || off < 1 || on > 3600000L || off > 3600000L) {
    out += F("ERR: times must be 1..3600000 ms\n");
    return;
  }
  if (!ensureOutput(g, out)) return;
  if (st[g].pwm) detachPwm(g, out);
  st[g].blink = true;
  st[g].onMs  = (uint32_t)on;
  st[g].offMs = (uint32_t)off;
  st[g].level = 1;
  digitalWrite(g, HIGH);
  st[g].due = millis() + (uint32_t)on;
  addf(out, "ok: gpio %u blink %ld ms on / %ld ms off", (unsigned)g, on, off);
}

static void cmdToggle(uint8_t g, String &out) {
  if (!ensureOutput(g, out)) return;
  if (st[g].blink) { st[g].blink = false; addf(out, "note: blink stopped on gpio %u\n", (unsigned)g); }
  if (st[g].pwm)   detachPwm(g, out);
  st[g].level = st[g].level ? 0 : 1;
  digitalWrite(g, st[g].level);
  addf(out, "ok: gpio %u = %u", (unsigned)g, (unsigned)st[g].level);
}

static void cmdStop(uint8_t g, String &out) {
  bool had = st[g].pwm || st[g].blink;
  if (st[g].pwm) detachPwm(g, out);
  st[g].blink = false;
  if (!had) addf(out, "ok: gpio %u had nothing running\n", (unsigned)g);
  else      addf(out, "ok: gpio %u stopped (mode stays %s)\n", (unsigned)g, modeName(st[g].mode));
}

static void cmdAll(const char *v, String &out) {
  if (!v) { out += F("ERR: usage: all <0|1>\n"); return; }
  uint8_t lvl = (v[0] == '1') ? 1 : (v[0] == '0') ? 0 : 2;
  if (lvl == 2) { out += F("ERR: level must be 0 or 1\n"); return; }
  int n = 0, stopped = 0, detached = 0;
  for (size_t i = 0; i < N_PINS; i++) {
    uint8_t g = PINS[i].gpio;
    if (st[g].mode != OUTPUT) continue;
    if (st[g].blink) { st[g].blink = false; stopped++; }
    if (st[g].pwm)   { ledcDetachPin(g);
                       if (st[g].ch >= 0 && st[g].ch < LEDC_MAX) ledcUsed[st[g].ch] = false;
                       st[g].pwm = false; st[g].ch = -1; detached++; }
    digitalWrite(g, lvl);
    st[g].level = lvl;
    n++;
  }
  if (n == 0) out += F("ERR: no pin is in mode out yet (use: mode <p> out)");
  else        addf(out, "ok: all %d output(s) <- %u%s%s", n, (unsigned)lvl,
                   stopped ? " (blink stopped)" : "", detached ? " (pwm off)" : "");
}

/* one line in, reply text out - used by telegram and by the serial console */
static void execCommand(const String &raw, String &out) {
  out = "";
  String s = raw;
  s.trim();
  while (s.startsWith("/")) s.remove(0, 1);
  if (s.length() == 0) { cmdHelp(out); return; }

  char buf[160];
  s.toCharArray(buf, sizeof(buf));

  char *cmd = strtok(buf, " \t");
  if (!cmd) { cmdHelp(out); return; }

  auto takePin = [&](uint8_t *g) -> bool {
    char *a = strtok(NULL, " \t");
    if (!a) { out += F("ERR: missing <p>\n"); return false; }
    char *end = NULL;
    long v = strtol(a, &end, 10);
    if (end == a || *end || v < 0 || v > 39) {
      addf(out, "ERR: '%s' is not a GPIO number 0..39\n", a);
      return false;
    }
    if (pinIndex((uint8_t)v) < 0) {
      addf(out, "ERR: gpio %ld is not available on this board (6-11 = flash, 37/38 n/a)\n", v);
      return false;
    }
    *g = (uint8_t)v;
    return true;
  };

  if (ciEq(cmd, "help") || ciEq(cmd, "start") || ciEq(cmd, "?") || ciEq(cmd, "h")) {
    cmdHelp(out); return;
  }
  if (ciEq(cmd, "pins") || ciEq(cmd, "list") || ciEq(cmd, "status")) { cmdPins(out); return; }
  if (ciEq(cmd, "stat") || ciEq(cmd, "info"))                        { cmdStat(out); return; }

  if (ciEq(cmd, "mode")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdMode(g, strtok(NULL, " \t"), out);
    return;
  }
  if (ciEq(cmd, "w") || ciEq(cmd, "write") || ciEq(cmd, "on") || ciEq(cmd, "off")) {
    uint8_t g;
    if (!takePin(&g)) return;
    const char *v = strtok(NULL, " \t");
    if (!v) v = ciEq(cmd, "on") ? "1" : ciEq(cmd, "off") ? "0" : NULL;
    cmdWrite(g, v, out);
    return;
  }
  if (ciEq(cmd, "r") || ciEq(cmd, "read") || ciEq(cmd, "digitalread")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdRead(g, out);
    return;
  }
  if (ciEq(cmd, "a") || ciEq(cmd, "adc") || ciEq(cmd, "analog")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdAnalog(g, out);
    return;
  }
  if (ciEq(cmd, "pwm")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdPwm(g, strtok(NULL, " \t"), strtok(NULL, " \t"), out);
    return;
  }
  if (ciEq(cmd, "blink")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdBlink(g, strtok(NULL, " \t"), strtok(NULL, " \t"), out);
    return;
  }
  if (ciEq(cmd, "tog") || ciEq(cmd, "toggle")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdToggle(g, out);
    return;
  }
  if (ciEq(cmd, "stop")) {
    uint8_t g;
    if (!takePin(&g)) return;
    cmdStop(g, out);
    return;
  }
  if (ciEq(cmd, "all")) {
    cmdAll(strtok(NULL, " \t"), out);
    return;
  }

  addf(out, "ERR: unknown command '%s' - try /help", cmd);
}

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

/* ======================================================================== *
 *  telegram transport - copied from 04TelegramRXTX
 * ======================================================================== */
enum WifiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED };

static WifiState wifiState = WIFI_IDLE;
static uint32_t connStartMs = 0;
static uint32_t retryAtMs = 0;
static bool linkLostPrinted = false;
static bool webhookCleared = false;
static bool bootTasksDone = false;

static uint32_t updateOffset = 0;
static uint32_t updatesSeen = 0;

static void heapWarnIfLow() {
  if (ESP.getMaxAllocHeap() < HEAP_WARN_BYTES) {
    Serial.println(F("WARN: largest free block < 40 KB - TLS may fail"));
  }
}

static String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

static bool telegramSend(const String &text) {
  if (wifiState != WIFI_CONNECTED) {
    Serial.println(F("telegram: wifi not connected - message skipped"));
    return false;
  }

  String url = String(F("https://api.telegram.org/bot")) + BOT_TOKEN + F("/sendMessage");
  String payload = String(F("{\"chat_id\":\"")) + jsonEscape(CHAT_ID) +
                   F("\",\"text\":\"") + jsonEscape(text) + F("\"}");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) {
    Serial.println(F("telegram: HTTPClient begin failed"));
    return false;
  }
  http.addHeader(F("Content-Type"), F("application/json"));

  int code = http.POST(payload);
  String body = http.getString();
  http.end();

  bool ok = (code == HTTP_CODE_OK) && body.startsWith(F("{\"ok\":true"));
  Serial.printf("telegram send -> HTTP %d %s (heap free=%u min=%u maxblk=%u)\n", code,
                ok ? "ok" : "FAIL", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  heapWarnIfLow();
  Serial.printf("telegram body: %.300s\n", body.c_str());
  return ok;
}

static int telegramApi(const String &args, String &body) {
  String url = String(F("https://api.telegram.org/bot")) + BOT_TOKEN + args;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  if (!http.begin(client, url)) {
    body = "HTTPClient begin failed";
    return -1;
  }
  int code = http.GET();
  body = http.getString();
  http.end();
  return code;
}

/* only CHAT_ID may drive the gpio - numeric id, or @username for a channel */
static bool authorized(long chatId, const char *chatUser) {
  if (CHAT_ID[0] == '@') {
    if (!chatUser) return false;
    return strcasecmp(chatUser, CHAT_ID + 1) == 0;
  }
  return String(chatId) == CHAT_ID;
}

/* returns true when a command was executed; reply (if any) is copied out */
static bool handleUpdate(JsonObject upd, String &reply) {
  uint32_t uid = upd["update_id"] | 0U;
  if (uid) updateOffset = uid + 1;
  updatesSeen++;

  static const char *MSG_KEYS[] = {"message", "edited_message", "channel_post",
                                   "edited_channel_post"};
  for (const char *k : MSG_KEYS) {
    if (!upd[k].is<JsonObject>()) continue;
    JsonObject m = upd[k];

    const char *text = m["text"];
    if (!text) text = m["caption"];

    long chatId = m["chat"]["id"] | 0L;
    const char *chatTitle = m["chat"]["title"];
    const char *chatUser = m["chat"]["username"];
    long fromId = m["from"]["id"] | 0L;
    const char *fromName = m["from"]["username"];
    if (!fromName) fromName = m["from"]["first_name"];

    Serial.printf("TG RX: update %u [%s]", (unsigned)uid, k);
    if (fromId) Serial.printf(" from %s (id %ld)", fromName ? fromName : "?", fromId);
    if (chatId) Serial.printf(" chat %ld", chatId);
    if (chatTitle) Serial.printf(" \"%s\"", chatTitle);
    if (text && text[0]) Serial.printf(" : %s\n", text);
    else Serial.println(F(" : <non-text message>"));

    if (!text || !text[0]) return false;
    if (!authorized(chatId, chatUser)) {
      Serial.println(F("TG: ignored - chat is not the owner"));
      return false;
    }
    execCommand(String(text), reply);
    return true;
  }

  const char *kind = nullptr;
  for (JsonPair kv : upd) {
    const char *key = kv.key().c_str();
    if (key && strcmp(key, "update_id") != 0) { kind = key; break; }
  }
  Serial.printf("TG RX: update %u type=%s (not displayed)\n", (unsigned)uid, kind ? kind : "?");
  return false;
}

static void addMsgFilter(JsonObject dst) {
  dst["text"] = true;
  dst["caption"] = true;
  JsonObject from = dst["from"].to<JsonObject>();
  from["id"] = true;
  from["username"] = true;
  from["first_name"] = true;
  JsonObject chat = dst["chat"].to<JsonObject>();
  chat["id"] = true;
  chat["title"] = true;
  chat["username"] = true;
}

static const JsonDocument &updateFilter() {
  static JsonDocument filter;
  static bool ready = false;
  if (!ready) {
    ready = true;
    filter["ok"] = true;
    JsonObject elem = filter["result"].to<JsonArray>().add<JsonObject>();
    elem["update_id"] = true;
    addMsgFilter(elem["message"].to<JsonObject>());
    addMsgFilter(elem["edited_message"].to<JsonObject>());
    addMsgFilter(elem["channel_post"].to<JsonObject>());
    addMsgFilter(elem["edited_channel_post"].to<JsonObject>());
  }
  return filter;
}

static void pollUpdates() {
  String args = String(F("/getUpdates?timeout=")) + POLL_TIMEOUT_S +
                F("&limit=") + POLL_LIMIT + F("&offset=") + updateOffset;
  String reply;
  bool doReply = false;
  uint32_t n = 0;

  {
    String body;
    int code = telegramApi(args, body);

    if (code != HTTP_CODE_OK) {
      Serial.printf("poll failed: HTTP %d %.200s\n", code, body.c_str());
      heapWarnIfLow();
      return;
    }

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(updateFilter()));
    if (err) {
      Serial.printf("poll json error: %s\n", err.c_str());
      return;
    }
    if (!(doc["ok"] | false)) {
      Serial.printf("poll api error: %s\n", doc["description"] | "?");
      return;
    }

    for (JsonObject upd : doc["result"].as<JsonArray>()) {
      String r;
      if (handleUpdate(upd, r)) {
        if (reply.length()) reply += F("\n----\n");
        reply += r;
        doReply = true;
      }
      n++;
    }
    Serial.printf("poll: %u new (total %u seen, offset %u) heap free=%u min=%u maxblk=%u\n",
                  (unsigned)n, (unsigned)updatesSeen, (unsigned)updateOffset,
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap());
  }  // body + doc gone

  heapWarnIfLow();
  if (doReply) {
    Serial.print(F("TG TX: "));
    Serial.println(reply);
    telegramSend(reply);
  }
}

/* ---------------- wifi ---------------- */
static void connectNow() {
  Serial.printf("connecting to \"%s\" ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiState = WIFI_CONNECTING;
  connStartMs = millis();
}

static void onConnected() {
  wifiState = WIFI_CONNECTED;
  linkLostPrinted = false;
  Serial.printf("connected to \"%s\" ip=%s rssi=%d dBm\n", WIFI_SSID,
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());

  if (!webhookCleared) {
    webhookCleared = true;
    String body;
    int code = telegramApi(F("/deleteWebhook"), body);
    if (code == HTTP_CODE_OK && (body.indexOf(F("\"ok\":true")) >= 0)) {
      Serial.println(F("webhook cleared - getUpdates polling enabled"));
    } else {
      Serial.printf("deleteWebhook -> HTTP %d %.200s\n", code, body.c_str());
    }
  }
}

/* ---------------- serial console: same commands as telegram ------------- */
static void handleSerial() {
  static char buf[TX_LINE_MAX];
  static size_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = '\0';
      len = 0;
      if (buf[0]) {
        String out;
        execCommand(String(buf), out);
        Serial.println(out);
      }
      continue;
    }
    if (c == 8 || c == 127) {
      if (len > 0) len--;
      continue;
    }
    if (len < TX_LINE_MAX - 1) buf[len++] = c;
  }
}

/* ---------------- led heartbeat: lub-dub ... pause ---------------- */
static void updateLed(uint32_t now) {
  static const uint16_t PHASE_MS[4] = {90, 160, 90, 1100};
  static uint8_t phase = 0;
  static uint32_t phaseStart = 0;
  if (phaseStart == 0) phaseStart = now;
  if (now - phaseStart >= PHASE_MS[phase]) {
    phase = (phase + 1) & 3;
    phaseStart = now;
  }
  digitalWrite(LED_BUILTIN, (phase == 0 || phase == 2) ? HIGH : LOW);
}

static Ticker ledTicker;
static void ledTick() {
  if (st[LED_BUILTIN].mode != -1) return;  // gpio2 handed to the user
  updateLed(millis());
}

/* ---------------- main ---------------- */
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  for (size_t i = 0; i < N_PINS; i++) {
    State &s = st[PINS[i].gpio];
    memset(&s, 0, sizeof(s));
    s.mode = -1;
    s.ch   = -1;
  }
  for (uint8_t i = 0; i < LEDC_MAX; i++) ledcUsed[i] = false;

  ledTicker.attach_ms(10, ledTick);
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("=== 06GPIOTelegram ==="));
  Serial.printf("ssid: %s  chat: %s (hardcoded)\n", WIFI_SSID, CHAT_ID);
  Serial.printf("%u gpio ready (6-11 flash, 37/38 n/a, EN untouched)\n", (unsigned)N_PINS);
  Serial.println(F("telegram: send /help  |  serial: type a command + Enter"));
  connectNow();
}

void loop() {
  handleSerial();

  if (wifiState == WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      onConnected();
    } else if ((int32_t)(millis() - connStartMs) > (int32_t)CONNECT_TIMEOUT_MS) {
      Serial.println(F("connect timeout - retrying"));
      wifiState = WIFI_IDLE;
      retryAtMs = millis() + RETRY_DELAY_MS;
    }
  } else if (wifiState == WIFI_IDLE) {
    if (retryAtMs && (int32_t)(millis() - retryAtMs) >= 0) {
      retryAtMs = 0;
      connectNow();
    }
  } else {  // WIFI_CONNECTED
    if (WiFi.status() != WL_CONNECTED) {
      if (!linkLostPrinted) {
        Serial.println(F("WiFi link lost - reconnecting ..."));
        linkLostPrinted = true;
        wifiState = WIFI_CONNECTING;
        connStartMs = millis();
      }
    } else {
      if (!bootTasksDone) {
        bootTasksDone = true;
        String hello = String(F("06GPIOTelegram online: ssid=")) + WIFI_SSID +
                       F(" ip=") + WiFi.localIP().toString() +
                       F(" rssi=") + WiFi.RSSI() + F(" dBm\n") +
                       F("gpio ready - send /help to control the pins");
        telegramSend(hello);
      }
      pollUpdates();
    }
  }

  serviceBlink();
  delay(5);
}
