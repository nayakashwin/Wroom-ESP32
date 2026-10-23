// --------------------------------------------------------------- secrets
//
// Copy this file to secrets.h in the same folder and fill in your own
// values. secrets.h is gitignored, so it is never pushed to GitHub - the
// example file is the only thing that is committed.
//
//     cp _secrets/secrets.example.h _secrets/secrets.h
//
// Every project that needs them includes the file like this (01Wifi,
// 02TelegramTX, 03TelegramRX, 04TelegramRXTX, 06GPIOTelegram,
// 08BlueToothTelegram, 15ESPHJ1602ALCD16pinTemp):
//
//     #if !defined(__has_include) || !__has_include("secrets.h")
//     #error "secrets.h not found - copy _secrets/secrets.example.h to _secrets/secrets.h and fill it in"
//     #endif
//     #include "secrets.h"
//
// and its platformio.ini puts this folder on the include path:
//
//     build_flags  = -D LED_BUILTIN=2 -I../_secrets
//
// Which project uses what:
//
//   01Wifi              WIFI_SSID, WIFI_PASS
//   02TelegramTX        + BOT_TOKEN, CHAT_ID
//   03TelegramRX        + BOT_TOKEN
//   04TelegramRXTX      + BOT_TOKEN, CHAT_ID
//   06GPIOTelegram      + BOT_TOKEN, CHAT_ID
//   08BlueToothTelegram + BOT_TOKEN, CHAT_ID
//   15ESPHJ1602ALCD16pinTemp WIFI_SSID, WIFI_PASS
//   05GPIO, 07BlueToothTempSensor - no secrets at all
//
#pragma once

// ---- WiFi ---------------------------------------------------------------
// The network the board joins. 2.4 GHz only - the ESP32 has no 5 GHz radio.
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"

// ---- Telegram -----------------------------------------------------------
// 1. Message @BotFather on Telegram, send /newbot, pick a name and a
//    username ending in "bot". It replies with a token like
//    123456789:AAExampleTokenExampleTokenExample
#define BOT_TOKEN "123456789:AAExampleTokenExampleTokenExample"

// 2. Send any message to your new bot (or add it to a group and talk to it),
//    then open https://api.telegram.org/bot<YOUR-BOT-TOKEN>/getUpdates in a
//    browser. Your chat id is the "id" inside "message" -> "chat" -> "id".
//    For a group or channel use the negative id Telegram shows (e.g.
//    -1001234567890), or the @channelusername.
#define CHAT_ID "123456789"
