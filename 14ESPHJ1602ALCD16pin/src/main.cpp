/* 14ESPHJ1602ALCD16pin — ESP32 serial-to-1602 LCD with line-by-line scroll.

Reads text from Serial (115200 baud, newline-terminated), displays on an
HJ1602A 16x2 character LCD (HD44780-compatible, 4-bit parallel).

Display behaviour:
  <=16 chars   -> centred on row 0
  longer text  -> word-wrapped into 16-char lines (words longer than 16
                  are hard-split); 1-2 lines shown statically, >2 lines
                  scroll upward one line at a time (2.5 s/line): the
                  top line moves off, the next line enters at the bottom,
                  1 s pause after the last line, then repeat

Wiring (NodeMCU-32S pinout):
  LCD RS  → GPIO4
  LCD E   → GPIO5
  LCD D4  → GPIO14
  LCD D5  → GPIO12
  LCD D6  → GPIO13
  LCD D7  → GPIO15
  LCD RW  → GND  (hard-wired for write-only)
  LCD VSS → GND
  LCD VDD → 3.3 V
  LCD V0  → 10 kΩ pot (VDD↔GND wiper to V0 for contrast)
  LCD A   → 3.3 V (backlight anode; some modules need a ~220 Ω series R)
  LCD K   → GND  (backlight cathode)

Requires the LiquidCrystal library (arduino-libraries/LiquidCrystal).
*/

#include <Arduino.h>
#include <LiquidCrystal.h>

// HJ1602A 16×2 LCD (parallel, 4-bit mode)
LiquidCrystal lcd(4, 5, 14, 12, 13, 15);

#define LCD_COLS 16
#define LCD_ROWS 2
#define MAX_MSG 128
#define MAX_LINES MAX_MSG

char msgBuf[MAX_MSG];
int msgLen = 0;

// Word-wrapped lines: start index + length into msgBuf
int lineStart[MAX_LINES];
int lineLen[MAX_LINES];
int lineCount = 0;

// Vertical scroll state machine
bool doScroll = false;
int topLine = 0;             // first line currently visible
unsigned long scrollMs = 0;
const unsigned long LINE_MS = 2500;   // ms per upward line shift
const unsigned long PAUSE_MS = 1000;  // ms pause after the last line

void wrapMessage();
void drawFrame();
void processMessage();

void setup() {
  Serial.begin(115200);
  lcd.begin(LCD_COLS, LCD_ROWS);
  lcd.clear();
  lcd.print("14 ESP32 LCD");
  Serial.println("ready - send text");
}

void loop() {
  // --- read serial input ---
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (msgLen > 0) {
        msgBuf[msgLen] = '\0';
        processMessage();
        msgLen = 0;
      }
    } else if (msgLen < MAX_MSG - 1) {
      msgBuf[msgLen++] = c;
    }
  }

  // --- advance scroll timer ---
  if (doScroll) {
    unsigned long now = millis();
    if (topLine < lineCount - 1) {
      if (now - scrollMs >= LINE_MS) {
        scrollMs = now;
        topLine++;
        drawFrame();
      }
    } else {
      // Last line has scrolled off the top — pause, then restart
      if (now - scrollMs >= PAUSE_MS) {
        scrollMs = now;
        topLine = 0;
        drawFrame();
      }
    }
  }
}

/* Wrap msgBuf into 16-char lines, breaking at spaces where possible.
   A single word wider than the row is hard-split at the column edge. */
void wrapMessage() {
  lineCount = 0;
  int i = 0;
  while (i < msgLen && lineCount < MAX_LINES) {
    while (i < msgLen && msgBuf[i] == ' ') i++;
    if (i >= msgLen) break;

    int start = i;
    int end = i;  // exclusive end of the current line
    while (i < msgLen) {
      int ws = i;
      while (i < msgLen && msgBuf[i] != ' ') i++;
      int wlen = i - ws;
      int candidate = (end == start) ? wlen : (end - start) + 1 + wlen;
      if (candidate <= LCD_COLS) {
        end = i;
      } else {
        if (end == start) end = start + LCD_COLS;  // hard-split long word
        break;
      }
      while (i < msgLen && msgBuf[i] == ' ') i++;
    }
    lineStart[lineCount] = start;
    lineLen[lineCount] = end - start;
    lineCount++;
    i = end;
  }
}

/* Render the two visible lines (topLine, topLine+1), padded to 16 cols. */
void drawFrame() {
  for (int r = 0; r < LCD_ROWS; r++) {
    lcd.setCursor(0, r);
    int l = topLine + r;
    int len = (l < lineCount) ? lineLen[l] : 0;
    int s = (l < lineCount) ? lineStart[l] : 0;
    for (int c = 0; c < len; c++) lcd.print(msgBuf[s + c]);
    for (int c = len; c < LCD_COLS; c++) lcd.print(' ');
  }
}

/* Process a received newline-terminated message. */
void processMessage() {
  // Strip trailing whitespace
  while (msgLen > 0 && isspace(msgBuf[msgLen - 1])) {
    msgLen--;
  }
  msgBuf[msgLen] = '\0';

  Serial.print("msgLen: ");
  Serial.println(msgLen);

  if (msgLen == 0) {
    lcd.clear();
    doScroll = false;
    return;
  }

  wrapMessage();
  topLine = 0;
  doScroll = false;

  if (msgLen <= LCD_COLS && lineCount == 1) {
    // Short: centred on row 0
    lcd.clear();
    lcd.setCursor((LCD_COLS - msgLen) / 2, 0);
    lcd.print(msgBuf);
    return;
  }

  drawFrame();

  if (lineCount > LCD_ROWS) {
    Serial.println("Scrolling line by line");
    scrollMs = millis();
    doScroll = true;
  }
}
