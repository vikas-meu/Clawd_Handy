/*
 * Copyright (C) 2026-Present Vikash Singh Thakur
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY.
 */
#include "Adafruit_PyCamera.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <SdFat.h> // Already included via Adafruit_PyCamera, but ensure
#include <driver/ledc.h>
#include <driver/gpio.h>
#include <Wire.h>
#include <time.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <vector>
#include <strings.h>
#include <esp_camera.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <ESPmDNS.h>
#include <qrcode.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "img_converters.h"
#include <driver/i2s_pdm.h>
#include "mp3dec.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "person_model.h"
// Claude screens (declared up here so Arduino's auto-generated prototypes can see them)
enum Face { FACE_NORMAL, FACE_BLINK, FACE_HAPPY, FACE_WORRIED, FACE_DEAD, FACE_LOOK_LEFT, FACE_LOOK_RIGHT };
enum NetJob { JOB_NONE, JOB_USAGE, JOB_ASK_IMAGE, JOB_FACE, JOB_WEB, JOB_QR, JOB_LOCATION, JOB_LISTEN, JOB_TALK,
              JOB_NOWPLAY, JOB_ART, JOB_MEDIA, JOB_BROWSE, JOB_HUNT, JOB_LISTEN2 };
enum TalkState { TALK_IDLE, TALK_LISTENING, TALK_THINKING, TALK_ANSWER };
enum AskState { ASK_PREVIEW, ASK_THINKING, ASK_ANSWER };
struct Alarm { bool on; uint8_t hour, minute, repeat; };
// What Clawds say to each other over ESP-NOW (Friends, chat, Pong Duel)
struct __attribute__((packed)) ClawdPacket {
  char magic[4]; // "CLWD"
  uint8_t type;
  char name[13];
  uint8_t food, energy, fun, flags; // flags bit 0: waiting in the Pong Duel lobby
  int16_t a, b, c, d, e, f;
  char text[24];
};
// ===================== YOUR SETTINGS =====================
// Put your own values here. Lines ending in "// private" are swapped back to these placeholders
// whenever you commit (see tools/private_values.py), so they never reach GitHub; your copy keeps
// them. If you use your phone's hotspot, set it to 2.4 GHz ("Extend compatibility").
#define WIFI_SSID "your-wifi-name" // private
#define WIFI_PASSWORD "your-wifi-password" // private
// Telegram bot (Home Assistant mode). Leave as-is if you don't use it.
#define BOTtoken "123456789:your-telegram-bot-token" // private
#define CHAT_ID "your-telegram-chat-id" // private
// The Clawd Phone app (android_companion/) is the bridge. The board finds it by itself, on home
// WiFi or on the phone's own hotspot, and pairs when you tap "Pair handheld" in the app.
// Only fill these in if automatic finding or pairing doesn't work for you.
#define BRIDGE_URL "" // private  e.g. "http://192.168.1.42:8765"
#define BRIDGE_TOKEN "" // private  the pairing token shown in the app
// Daily Feed > YouTube: your channel's handle, and a free YouTube Data API v3 key from
// console.cloud.google.com (APIs & Services > Library > YouTube Data API v3 > Credentials)
#define YT_HANDLE "@yourchannel" // private
#define YT_API_KEY "your-youtube-api-key" // private
// Daily Feed > Currency: rates are shown in this currency
#define HOME_CURRENCY "INR"
// Friends: your Clawd's name as other boards see it ("" = Clawd-XXXX from the board's address)
#define CLAWD_NAME ""
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
// Pin definitions
#define MIC 20
#define SPEAKER 18
#define AWEXP_SPKR_SD 0 // Speaker mute control on AW9523 pin 0
#define SDA_PIN 34
#define SCL_PIN 33
// I2C slave address for Arduino
const uint8_t ARDUINO_ADDR = 0x08;
const uint8_t ARDUINO2_ADDR = 0x09; // Second Arduino
// Timezone offset for IST (UTC+5:30)
const long timezoneOffset = 19800; // 5.5 * 3600
// Controller ESP32 IP (replace with the actual IP of the ESP32 running the web server)
const char* controllerIP = "192.168.1.100"; // TODO: Set to the IP address of the control ESP32
Adafruit_PyCamera pycamera;
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);
// Global variables for the "chunked" upload
int currentByte = 0;
size_t fb_length = 0;
uint8_t* fb_buffer = NULL;
// Helper functions for Telegram binary transfer
bool isMoreDataAvailable() {
  return (fb_length - currentByte);
}
uint8_t photoNextByte() {
  currentByte++;
  return fb_buffer[currentByte - 1];
}
// For Telegram polling
unsigned long bot_lasttime = 0;
const long BOT_MTBS = 2000; // Poll every 1 second
// For motion detection
uint16_t *prev_buffer = NULL;
unsigned long last_snap_time = 0;
const long SNAP_COOLDOWN = 5000; // 5 seconds cooldown to avoid spamming
bool securityMode = false;
// For green object tracking
bool tracking = false;
// For scheduling
struct Event {
  time_t eventTime;
  String details;
  String arduinoCmd;
  String reminder;
  bool triggered = false;
};
std::vector<Event> scheduledEvents;
bool awaitingSchedule = false;
// Modes
enum Mode {
  MODE_IDLE,
  MODE_MENU,
  MODE_CAMERA,
  MODE_GAME_MENU,
  MODE_GAME,
  MODE_HOME_ASSISTANT,
  MODE_USAGE,
  MODE_ASK,
  MODE_DANCE,
  MODE_SLEEP,
  MODE_ALARMS,
  MODE_ALARM_RING,
  MODE_FACE,
  MODE_WORLD,
  MODE_PET,
  MODE_TOOLS,
  MODE_TOOL,
  MODE_TALK,
  MODE_RETRO,
  MODE_NOWPLAY,
  MODE_BROWSER,
  MODE_MUSIC,
  MODE_FRIENDS
};
Mode currentMode = MODE_IDLE;
int menuSelection = 0;
const char* menuOptions[] = {"Clawd Talk", "Claude Ask", "API Status", "Claude Dance", "Clawd Pet", "Alarms",
                             "Face Finder", "World Live", "Daily Feed", "Retro Cam", "Now Playing", "Browser",
                             "Music", "Friends", "Games", "Toolbox", "Camera Mode", "Home Assistant"};
int numMenuOptions = sizeof(menuOptions) / sizeof(menuOptions[0]);
// Function prototypes
void drawMenu();
void handleMenuNavigation();
void cameraMode();
void homeAssistantMode();
void handleNewMessages(int numNewMessages);
void takeAndSendPhoto(int reply_to_message_id = 0);
void recordAudio();
void playAudio();
void sendDanceCommand();
void trackGreenObject();
void checkScheduledEvents();
bool parseSchedule(String text, String chat_id);
void sendI2CCommandTo(uint8_t addr, const char* cmd);
void sendTelegramMessage(String chat_id, String message, int reply_to_message_id = 0);
void sendControlCommand(String endpoint);
// ===================== CLAUDE THEME + SHARED CANVAS =====================
#define CLAUDE_ORANGE 0xDBAA // #D97757 in RGB565
#define CLAUDE_DARK   0x1082 // #141413
#define CLAUDE_CREAM  0xFFDE // #FAF9F5
#define CLAUDE_GRAY   0x8C51 // #8A8A8A
#define CLAUDE_TRACK  0x2965 // #2E2D2A  empty part of a bar
#define CLAUDE_AMBER  0xED07 // #E8A23A
#define CLAUDE_RED    0xE249 // #E5484D
#define CLAUDE_GREEN  0x4D6A // #4CAF50
#define CLAUDE_BLUE   0x5D1F // #5BA3F8  sweat drop
#define BOT_U 14             // size of one bot "pixel" block on the big screens
// pycamera.fb has no buffer until the first camera capture, so all Claude screens
// draw into their own PSRAM canvas and push it straight to the display
PyCameraFB *ui = NULL;
bool initUi() {
  uint16_t *buf = (uint16_t *)ps_malloc(240 * 240 * sizeof(uint16_t));
  if (!buf) return false;
  ui = new PyCameraFB(240, 240);
  ui->setFB(buf);
  return true;
}
void pushUi() {
  pycamera.drawRGBBitmap(0, 0, ui->getBuffer(), 240, 240);
}
float easeOutBack(float t) {
  const float c1 = 1.70158f, c3 = c1 + 1.0f;
  return 1.0f + c3 * powf(t - 1.0f, 3) + c1 * powf(t - 1.0f, 2);
}
// Claude "spark": 12 rounded rays of slightly uneven length around a centre
void drawClaudeLogo(Adafruit_GFX *g, int cx, int cy, float r, float rotDeg, uint16_t color) {
  if (r < 2) return;
  static const float lens[12] = {1.00, 0.80, 0.94, 0.76, 1.00, 0.84, 0.90, 0.78, 1.00, 0.82, 0.96, 0.77};
  static const float jitter[12] = {0, 4, -3, 2, -2, 5, 0, -4, 3, -1, 2, -3};
  float hw = r * 0.085f;
  for (int i = 0; i < 12; i++) {
    float a = (i * 30.0f + jitter[i] + rotDeg) * DEG_TO_RAD;
    float ca = cosf(a), sa = sinf(a);
    float px = -sa, py = ca; // perpendicular to the ray
    float len = r * lens[i];
    float bw = hw * 1.15f, tw = hw * 0.85f;
    int x0 = cx + px * bw, y0 = cy + py * bw;
    int x1 = cx - px * bw, y1 = cy - py * bw;
    int tx = cx + ca * len, ty = cy + sa * len;
    int x2 = tx - px * tw, y2 = ty - py * tw;
    int x3 = tx + px * tw, y3 = ty + py * tw;
    g->fillTriangle(x0, y0, x1, y1, x2, y2, color);
    g->fillTriangle(x0, y0, x2, y2, x3, y3, color);
    g->fillCircle(tx, ty, max(1, (int)tw), color);
  }
  g->fillCircle(cx, cy, r * 0.2f, color);
}
// Pixel bot: 7x4 block body, two eyes, side arms, four legs; (x, y) = top-left of the body.
// armPose / leftArm: 0 = down, 1 = out, 2 = raised (1 <-> 2 looks like waving)
void drawBot(Adafruit_GFX *g, int x, int y, int U, Face face, int armPose, int leftArm) {
  g->fillRect(x, y, 7 * U, 4 * U, CLAUDE_ORANGE);
  int shift = face == FACE_LOOK_LEFT ? -U / 3 : face == FACE_LOOK_RIGHT ? U / 3 : 0;
  int thick = max(2, U / 4);
  for (int i = 0; i < 2; i++) {
    int ex = x + (i == 0 ? U : 5 * U) + shift, ey = y + U * 3 / 4;
    switch (face) {
      case FACE_BLINK:
        g->fillRect(ex, ey + U * 3 / 4, U, thick, CLAUDE_DARK);
        break;
      case FACE_HAPPY: // ^ ^
        g->fillTriangle(ex, ey + U, ex + U / 2, ey + U / 4, ex + U, ey + U, CLAUDE_DARK);
        g->fillTriangle(ex + thick, ey + U + 1, ex + U / 2, ey + U / 4 + thick, ex + U - thick, ey + U + 1, CLAUDE_ORANGE);
        break;
      case FACE_DEAD: // x x
        for (int t = 0; t < thick; t++) {
          g->drawLine(ex + t, ey, ex + U - thick + t, ey + U, CLAUDE_DARK);
          g->drawLine(ex + U - 1 - t, ey, ex + thick - 1 - t, ey + U, CLAUDE_DARK);
        }
        break;
      case FACE_WORRIED:
        g->fillRect(ex, ey + U / 3, U, U, CLAUDE_DARK);
        break;
      default:
        g->fillRect(ex, ey, U, U * 3 / 2, CLAUDE_DARK);
        break;
    }
  }
  if (face == FACE_WORRIED) { // sweat drop
    int r = max(2, U / 3);
    g->fillCircle(x - U / 2, y + U / 2 + r, r, CLAUDE_BLUE);
    g->fillTriangle(x - U / 2 - r, y + U / 2 + r, x - U / 2 + r, y + U / 2 + r, x - U / 2, y + U / 2 - r, CLAUDE_BLUE);
  }
  // left arm
  if (leftArm == 0) {
    g->fillRect(x - U, y + 2 * U, U, U, CLAUDE_ORANGE);
  } else if (leftArm == 1) {
    g->fillRect(x - U, y + U, U, U, CLAUDE_ORANGE);
  } else {
    g->fillRect(x - U, y, U, U, CLAUDE_ORANGE);
    g->fillRect(x - 2 * U, y - U, U, U, CLAUDE_ORANGE);
  }
  // right (waving) arm
  if (armPose == 0) {
    g->fillRect(x + 7 * U, y + 2 * U, U, U, CLAUDE_ORANGE);
  } else if (armPose == 1) {
    g->fillRect(x + 7 * U, y + U, U, U, CLAUDE_ORANGE);
  } else {
    g->fillRect(x + 7 * U, y, U, U, CLAUDE_ORANGE);
    g->fillRect(x + 8 * U, y - U, U, U, CLAUDE_ORANGE);
  }
  // legs
  const int legX[4] = {U, 2 * U, U * 9 / 2, U * 11 / 2};
  for (int i = 0; i < 4; i++) g->fillRect(x + legX[i], y + 4 * U, max(2, U / 2), U, CLAUDE_ORANGE);
}
// Cream speech bubble whose tail points at (tx, ty): below, left of, or above the bubble
void drawBubble(Adafruit_GFX *g, int x, int y, int w, int h, int tx, int ty) {
  g->fillRoundRect(x, y, w, h, 10, CLAUDE_CREAM);
  if (ty >= y + h) {
    int bx = constrain(tx, x + 12, x + w - 26);
    g->fillTriangle(bx, y + h - 1, bx + 14, y + h - 1, tx, ty, CLAUDE_CREAM);
  } else if (ty < y) {
    int bx = constrain(tx, x + 12, x + w - 26);
    g->fillTriangle(bx, y, bx + 14, y, tx, ty, CLAUDE_CREAM);
  } else {
    int by = constrain(ty, y + 8, y + h - 20);
    g->fillTriangle(x, by, x, by + 12, tx, ty, CLAUDE_CREAM);
  }
}
// Word-wraps text into lines of at most maxChars; returns how many lines were written
int wrapText(const String &text, int maxChars, String *lines, int maxLines) {
  int count = 0;
  String line;
  int i = 0, n = text.length();
  while (i < n && count < maxLines) {
    int sp = text.indexOf(' ', i);
    if (sp < 0) sp = n;
    String word = text.substring(i, sp);
    i = sp + 1;
    while ((int)word.length() > maxChars) { // a word longer than a line: hard split
      if (line.length()) { lines[count++] = line; line = ""; }
      if (count >= maxLines) return count;
      lines[count++] = word.substring(0, maxChars);
      word = word.substring(maxChars);
      if (count >= maxLines) return count;
    }
    if (line.length() && (int)(line.length() + 1 + word.length()) > maxChars) {
      lines[count++] = line;
      line = word;
    } else {
      if (line.length()) line += ' ';
      line += word;
    }
  }
  if (line.length() && count < maxLines) lines[count++] = line;
  return count;
}

// ===================== BOOT SPLASH =====================
// Orange Claude logo pops in, then the orange pixel bot jumps up and waves "Hi!"
void playClaudeLogo() {
  // Pop + spin in
  const int frames = 28;
  for (int f = 0; f <= frames; f++) {
    float t = (float)f / frames;
    ui->fillScreen(CLAUDE_DARK);
    drawClaudeLogo(ui, 120, 100, 60.0f * easeOutBack(t), -120.0f * (1.0f - t), CLAUDE_ORANGE);
    pushUi();
  }
  pycamera.speaker_tone(880, 60);
  pycamera.speaker_tone(1320, 90);
  // Type "Claude" under the logo
  const char *word = "Claude";
  ui->setTextSize(3);
  ui->setTextColor(CLAUDE_CREAM);
  for (int n = 1; n <= 6; n++) {
    ui->setCursor(66, 180); // 6 chars * 18px = 108px wide, centred
    for (int i = 0; i < n; i++) ui->print(word[i]);
    pushUi();
    delay(70);
  }
  delay(900);
  // Spin + shrink out
  for (int f = 12; f >= 0; f--) {
    ui->fillScreen(CLAUDE_DARK);
    drawClaudeLogo(ui, 120, 100, 60.0f * f / 12.0f, 180.0f * (12 - f) / 12.0f, CLAUDE_ORANGE);
    pushUi();
  }
}
// Draws one frame of the bot scene; hiChars = how many letters of "Hi!" to show
void drawBotScene(int botY, Face face, int armPose, int hiChars) {
  const int botX = (240 - 7 * BOT_U) / 2;
  ui->fillScreen(CLAUDE_DARK);
  drawBot(ui, botX, botY, BOT_U, face, armPose, 0);
  if (hiChars > 0) {
    drawBubble(ui, 120, 30, 105, 52, 140, 100);
    ui->setTextSize(3);
    ui->setTextColor(CLAUDE_ORANGE);
    ui->setCursor(145, 44);
    const char *hi = "Hi!";
    for (int i = 0; i < hiChars; i++) ui->print(hi[i]);
  }
  pushUi();
}
void playBotHi() {
  const int restY = 120;
  // Jump up from below the screen
  const int frames = 20;
  for (int f = 0; f <= frames; f++) {
    float t = (float)f / frames;
    int y = 250 + (int)((restY - 250) * easeOutBack(t));
    drawBotScene(y, FACE_NORMAL, 0, 0);
  }
  pycamera.speaker_tone(660, 40);
  delay(200);
  // Speech bubble types "Hi!" while the arm comes up
  const int notes[3] = {880, 1100, 1320};
  for (int n = 1; n <= 3; n++) {
    drawBotScene(restY, FACE_HAPPY, 1, n);
    pycamera.speaker_tone(notes[n - 1], 40);
    delay(80);
  }
  // Wave 3 times, blink once in the middle
  for (int w = 0; w < 3; w++) {
    drawBotScene(restY, w == 1 ? FACE_BLINK : FACE_HAPPY, 2, 3);
    delay(160);
    drawBotScene(restY, FACE_HAPPY, 1, 3);
    delay(160);
  }
  drawBotScene(restY, FACE_NORMAL, 0, 3);
  delay(700);
}
void playBootSplash() {
  playClaudeLogo();
  playBotHi();
}

// ===================== BRIDGE NETWORKING =====================
// HTTP runs in a task on core 0 so animations keep playing on core 1 while we wait
volatile NetJob netJob = JOB_NONE; // job in progress (JOB_NONE = idle)
volatile bool netDone = false;     // worker finished, result waiting for serviceNetJob()
String netPath;
uint8_t *netBody = NULL;           // PSRAM buffer, freed when the job is serviced
size_t netBodyLen = 0;
const char *netContentType = "";
int netStatus = 0;
String netResponse;
uint8_t *artJpg = NULL; // album art JPEG from the phone (Now Playing)
size_t artJpgLen = 0;
TaskHandle_t netTaskHandle = NULL;
Preferences prefs;             // flash storage: alarms, clock, scores, pet, reminders, phone address
String bridgeUrl = BRIDGE_URL; // replaced automatically when the phone turns up at another IP
volatile bool bridgeMoved = false; // the net task found the phone somewhere new; loop() saves it
String bridgeToken = BRIDGE_TOKEN; // pairing token: from YOUR SETTINGS, or fetched when you pair
volatile bool bridgeTokenChanged = false;
// Free IP-based location, used when the phone can't share its GPS
#define IP_LOCATE_URL "https://ipwho.is/?fields=success,city,latitude,longitude"
// Broadcasts "where are you?" on the LAN; the phone app answers from its current IP.
bool discoverBridge() {
  WiFiUDP udp;
  if (!udp.begin(8767)) return false;
  udp.beginPacket(IPAddress(255, 255, 255, 255), 8766);
  udp.print("CLAUDE_BRIDGE?");
  udp.endPacket();
  bool found = false;
  unsigned long start = millis();
  while (!found && millis() - start < 1500) {
    if (udp.parsePacket()) {
      char reply[32] = {0};
      udp.read(reply, sizeof(reply) - 1);
      int port = 0;
      if (sscanf(reply, "CLAUDE_BRIDGE %d", &port) == 1) {
        String url = "http://" + udp.remoteIP().toString() + ":" + String(port);
        if (url != bridgeUrl) {
          bridgeUrl = url;
          bridgeMoved = true;
        }
        Serial.println("Found the phone at " + bridgeUrl);
        found = true;
      }
    }
    delay(20);
  }
  udp.stop();
  return found;
}
int bridgeRequest() {
  if (!bridgeUrl.length()) return -1;
  HTTPClient http;
  http.setConnectTimeout(3000);
  // Claude (and web searches) can take a while; listening lasts until you stop talking
  http.setTimeout(netJob == JOB_ASK_IMAGE || netJob == JOB_TALK || netJob == JOB_HUNT ? 75000
                  : netJob == JOB_LISTEN || netJob == JOB_LISTEN2 || netJob == JOB_BROWSE ? 30000
                                                                                        : 10000);
  int status = -1;
  if (http.begin(bridgeUrl + netPath)) {
    if (bridgeToken.length()) http.addHeader("X-Clawd-Token", bridgeToken);
    if (netBody) {
      http.addHeader("Content-Type", netContentType);
      status = http.POST(netBody, netBodyLen);
    } else {
      status = http.GET();
    }
    if (status == 200 && netJob == JOB_ART) { // binary JPEG, not JSON
      int len = http.getSize();
      if (len > 0 && len < 65536 && (artJpg = (uint8_t *)ps_malloc(len))) {
        artJpgLen = http.getStreamPtr()->readBytes(artJpg, len);
      }
    } else if (status > 0) {
      netResponse = http.getString();
    }
    http.end();
  }
  return status;
}
// On the phone's hotspot the phone is our gateway, and hotspots often swallow the discovery
// broadcast, so try the gateway's address directly
int gatewayRequest() {
  IPAddress gw = WiFi.gatewayIP();
  String url = "http://" + gw.toString() + ":8765";
  if (gw == IPAddress(0, 0, 0, 0) || url == bridgeUrl) return -1;
  String before = bridgeUrl;
  bridgeUrl = url;
  int status = bridgeRequest();
  if (status > 0 && netResponse.indexOf("\"ok\"") >= 0) { // it's the Clawd Phone app
    bridgeMoved = true;
    Serial.println("Found the phone at the gateway, " + bridgeUrl);
    return status;
  }
  bridgeUrl = before;
  netResponse = "";
  return -1;
}
// The app was refused our token (or we have none): ask for it. The phone only hands it out for
// two minutes after you tap "Pair handheld" (or switch the link on) in the app.
bool pairWithPhone() {
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  bool paired = false;
  if (http.begin(bridgeUrl + "/pair")) {
    if (http.GET() == 200) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getString()) && (doc["ok"] | false)) {
        String token = doc["token"] | "";
        if (token.length()) {
          bridgeToken = token;
          bridgeTokenChanged = true;
          paired = true;
          Serial.println("Paired with the phone");
        }
      }
    }
    http.end();
  }
  return paired;
}
// Plain HTTPS GET to a public web API (netPath holds the full URL)
int webRequest() {
  WiFiClientSecure tls;
  tls.setInsecure(); // these are public, read-only feeds
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  http.setUserAgent("ClaudeDeskBuddy/1.0");
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // Wikipedia's random article is a redirect
  int status = -1;
  if (http.begin(tls, netPath)) {
    status = http.GET();
    if (status > 0) netResponse = http.getString();
    http.end();
  }
  return status;
}
void netTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    netResponse = "";
    if (netJob == JOB_WEB) {
      netStatus = webRequest();
    } else {
      // Try the last known address first; only search for the phone when it isn't there
      netStatus = bridgeRequest();
      if (netStatus < 0 && discoverBridge()) netStatus = bridgeRequest();
      if (netStatus < 0) netStatus = gatewayRequest();
      if (netStatus == 401 && pairWithPhone()) {
        netResponse = "";
        netStatus = bridgeRequest();
      }
      // No phone GPS (app closed, or permission off): fall back to IP-based location
      if (netJob == JOB_LOCATION && (netStatus != 200 || netResponse.indexOf("\"success\":true") < 0)) {
        netResponse = "";
        netPath = IP_LOCATE_URL;
        netStatus = webRequest();
      }
    }
    netDone = true;
  }
}
bool netBusy() {
  return netJob != JOB_NONE;
}
bool startNetJob(NetJob job, const String &path, uint8_t *body, size_t len, const char *contentType) {
  if (netBusy() || WiFi.status() != WL_CONNECTED) return false;
  netPath = path;
  netBody = body;
  netBodyLen = len;
  netContentType = contentType;
  netDone = false;
  netJob = job;
  xTaskNotifyGive(netTaskHandle);
  return true;
}

// ===================== CLAUDE DANCE (and the alarm's music) =====================
// Clawbot dances to chiptune beats. A task on core 0 plays the music on the speaker;
// the animation reads which 16th-note step is playing and moves to it.
#define BOARD_SPEAKER_PIN 46 // the board's real speaker pin (this sketch's SPEAKER define is 18)
#define DANCE_LEDC_CHANNEL 6 // its own LEDC channel, so playAudio()'s setup is left alone
// A song is one bar of drums plus what plays after each hit, looped over 4 chords
struct Song {
  const char *name;
  int bpm;
  const char *drums; // K kick, S snare, H closed hat, O open hat, . nothing
  const char *tails; // R root, F fifth, U octave up, C chord stab, . rest
  float roots[4];    // bass notes sit an octave up so the little speaker can play them
  float chords[4][3];
};
const Song SONGS[] = {
  {"Boom bap", 92, "K.H.S.HKH.K.S.HO", "R..RC.F...R.C.U.",
   {220.0f, 174.6f, 196.0f, 164.8f}, // Am F G Em
   {{440.0f, 523.3f, 659.3f}, {349.2f, 440.0f, 523.3f}, {392.0f, 493.9f, 587.3f}, {329.6f, 392.0f, 493.9f}}},
  {"Funk wake-up", 112, "KHHKSHKHHKHHSHKO", "R.URC.RU.RF.C.U.",
   {164.8f, 164.8f, 220.0f, 196.0f}, // Em Em A G
   {{329.6f, 392.0f, 493.9f}, {293.7f, 392.0f, 493.9f}, {440.0f, 554.4f, 659.3f}, {392.0f, 493.9f, 587.3f}}},
  {"Lo-fi chill", 76, "K..HS..HK.KHS..H", "R..C..F.R..C.U..",
   {261.6f, 220.0f, 174.6f, 196.0f}, // C Am F G
   {{523.3f, 659.3f, 784.0f}, {440.0f, 523.3f, 659.3f}, {349.2f, 440.0f, 523.3f}, {392.0f, 493.9f, 587.3f}}},
};
const int SONG_COUNT = sizeof(SONGS) / sizeof(SONGS[0]);
#define SONG_ALARM 1
volatile bool danceOn = false;  // music should be playing
volatile bool danceIdle = true; // music task is parked and not touching the speaker
volatile int danceStep = -1;    // 16th-note step now playing (0..63 = 4 bars)
volatile unsigned long danceStepAt = 0;
volatile int danceBpm = 92;
volatile int danceSong = 0;
int danceMove = -1;             // -1 = new move every bar, otherwise a fixed move
TaskHandle_t danceTaskHandle = NULL;
const char *danceMoveNames[] = {"Bounce!", "Side step!", "Arm wave!", "Robot pop!"};
void danceTone(float hz) {
  ledcWriteTone(BOARD_SPEAKER_PIN, (uint32_t)hz);
}
void danceWait(int ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}
// Kick: a fast downward pitch sweep
void playKick() {
  for (int t = 0; t < 50 && danceOn; t += 2) {
    danceTone(220.0f * powf(0.3f, t / 50.0f));
    danceWait(2);
  }
}
// Snare and hats: random pitches hopping every millisecond sound like noise
void playNoise(int ms, int lo, int hi) {
  for (int t = 0; t < ms && danceOn; t++) {
    danceTone(random(lo, hi));
    danceWait(1);
  }
}
void playNote(float hz, int ms) {
  danceTone(hz);
  danceWait(ms);
}
// Chiptune-style chord: cycle quickly through its three notes
void playArp(const float *chord, int ms) {
  for (int t = 0, i = 0; t < ms && danceOn; t += 18, i++) {
    danceTone(chord[i % 3]);
    danceWait(min(18, ms - t));
  }
}
void danceTask(void *) {
  for (;;) {
    if (!danceOn) {
      danceIdle = true;
      danceWait(10);
      continue;
    }
    const Song &song = SONGS[danceSong];
    unsigned long start = millis();
    unsigned long stepMs = 15000UL / danceBpm; // one 16th note
    int step = (danceStep + 1) % 64;
    danceStepAt = start;
    danceStep = step;
    int bar = step / 16, s = step % 16;
    switch (song.drums[s]) {
      case 'K': playKick(); break;
      case 'S': playNoise(40, 1500, 7000); break;
      case 'H': playNoise(8, 6000, 10000); break;
      case 'O': playNoise(30, 5000, 10000); break;
    }
    // The rest of the step is bass or a chord stab, with a short gap so notes don't blur
    int left = (int)stepMs - (int)(millis() - start) - 12;
    if (left > 20 && danceOn) {
      switch (song.tails[s]) {
        case 'R': playNote(song.roots[bar], left); break;
        case 'F': playNote(song.roots[bar] * 1.5f, left); break;
        case 'U': playNote(song.roots[bar] * 2.0f, left); break;
        case 'C': playArp(song.chords[bar], left); break;
      }
    }
    danceTone(0);
    while (danceOn && millis() - start < stepMs) danceWait(1);
  }
}
void startDance(int song) {
  danceSong = song;
  danceBpm = SONGS[song].bpm;
  if (danceOn) return;
  // Every beep goes through tone(), which leaves the speaker pin attached to its own
  // channel. noTone() asks the core's tone task to let go; retry until it has.
  noTone(BOARD_SPEAKER_PIN);
  bool attached = false;
  for (int i = 0; i < 20 && !attached; i++) {
    delay(10);
    attached = ledcAttachChannel(BOARD_SPEAKER_PIN, 1000, 10, DANCE_LEDC_CHANNEL);
  }
  if (!attached) {
    Serial.println("Dance: speaker busy, dancing without music");
    return;
  }
  pycamera.aw.digitalWrite(AWEXP_SPKR_SD, HIGH); // un-mute
  danceStep = -1;
  danceIdle = false;
  danceOn = true;
}
void stopDance() {
  if (!danceOn) return;
  danceOn = false;
  unsigned long t = millis();
  while (!danceIdle && millis() - t < 500) delay(2); // let the current step finish
  ledcWriteTone(BOARD_SPEAKER_PIN, 0);
  ledcDetach(BOARD_SPEAKER_PIN);
  pycamera.aw.digitalWrite(AWEXP_SPKR_SD, LOW); // mute
}
// Which step of the music we're on, and how far through it (0..1)
void danceClock(int &step, float &phase) {
  unsigned long now = millis();
  unsigned long stepMs = 15000UL / danceBpm;
  if (danceOn && danceStep >= 0) {
    step = danceStep;
    phase = min(1.0f, (now - danceStepAt) / (float)stepMs);
  } else { // no music (speaker busy): keep dancing to the clock
    step = (now / stepMs) % 64;
    phase = (now % stepMs) / (float)stepMs;
  }
}
// Where the bot's body, arms and face should be for this moment of a dance move
void dancePose(int step, float phase, int move, int &dx, int &dy, int &leftArm, int &rightArm, Face &face) {
  int beatNum = step / 4;                   // quarter notes so far
  float beat = ((step % 4) + phase) / 4.0f; // 0..1 through the current beat
  dx = dy = leftArm = rightArm = 0;
  face = FACE_HAPPY;
  switch (move) {
    case 0: // Bounce: hop on every beat, arms take turns going up
      dy = -(int)(12 * sinf(beat * PI));
      leftArm = beatNum % 2 ? 2 : 0;
      rightArm = beatNum % 2 ? 0 : 2;
      break;
    case 1: { // Side step: sway over two beats, eyes lead the way
      float sway = sinf((beatNum % 2 + beat) * PI);
      dx = (int)(20 * sway);
      dy = (int)(4 * fabsf(sway));
      face = sway > 0.2f ? FACE_LOOK_RIGHT : sway < -0.2f ? FACE_LOOK_LEFT : FACE_HAPPY;
      leftArm = rightArm = 1;
      break;
    }
    case 2: // Arm wave: arms flap on every 8th note, blink on the snare
      leftArm = (step / 2) % 2 ? 2 : 1;
      rightArm = (step / 2) % 2 ? 1 : 2;
      dy = -(int)(4 * fabsf(sinf(beat * PI)));
      face = SONGS[danceSong].drums[step % 16] == 'S' ? FACE_BLINK : FACE_NORMAL;
      break;
    default: { // Robot pop: snap between poses on 8th notes, no in-between frames
      static const int8_t poses[4][4] = {{-10, 0, 2, 0}, {10, 4, 0, 2}, {0, -8, 2, 2}, {0, 6, 1, 1}};
      const int8_t *p = poses[(step / 2) % 4];
      dx = p[0];
      dy = p[1];
      leftArm = p[2];
      rightArm = p[3];
      face = (step / 2) % 4 == 2 ? FACE_BLINK : FACE_NORMAL;
      break;
    }
  }
}
void drawNote(int x, int y, uint16_t color) {
  ui->fillCircle(x, y, 4, color);
  ui->drawFastVLine(x + 3, y - 14, 14, color);
  ui->drawLine(x + 3, y - 14, x + 9, y - 10, color);
  ui->drawLine(x + 4, y - 14, x + 10, y - 10, color);
}
void drawDance() {
  unsigned long now = millis();
  int step;
  float phase;
  danceClock(step, phase);
  int s = step % 16;
  int move = danceMove >= 0 ? danceMove : (step / 16) % 4;
  const int U = BOT_U;
  const int baseX = (240 - 7 * U) / 2, baseY = 104;
  int dx, dy, leftArm, rightArm;
  Face face;
  dancePose(step, phase, move, dx, dy, leftArm, rightArm, face);
  int x = baseX + dx, y = baseY + dy;
  const char *drums = SONGS[danceSong].drums;
  bool kick = drums[s] == 'K', snare = drums[s] == 'S';
  ui->fillScreen(CLAUDE_DARK);
  // Disco spark: spins with the music, rings pulse out on kicks and snares
  drawClaudeLogo(ui, 120, 32, 18 + (kick ? 4 * (1 - phase) : 0), step * 15 + phase * 15, CLAUDE_ORANGE);
  if (kick && phase < 0.8f) ui->drawCircle(120, 32, 24 + (int)(phase * 40), CLAUDE_ORANGE);
  if (snare && phase < 0.8f) ui->drawCircle(120, 32, 24 + (int)(phase * 30), CLAUDE_CREAM);
  // Music notes floating up both sides
  for (int i = 0; i < 4; i++) {
    int nx = (i % 2 ? 216 : 12) + (int)(5 * sinf(now / 300.0f + i));
    int ny = 180 - (int)((now / 25 + i * 45) % 150);
    drawNote(nx, ny, i % 2 ? CLAUDE_ORANGE : CLAUDE_CREAM);
  }
  // Shadow shrinks as the bot jumps
  int sw = 90 - max(0, baseY - y) * 3;
  ui->fillRoundRect(x + 7 * U / 2 - sw / 2, 178, sw, 6, 3, CLAUDE_TRACK);
  drawBot(ui, x, y, U, face, rightArm, leftArm);
  // Dance floor: one tile lights up per 16th note
  for (int i = 0; i < 8; i++) {
    uint16_t c = i == step % 8 ? CLAUDE_ORANGE : (i + step / 4) % 2 ? CLAUDE_TRACK : 0x18C3;
    ui->fillRect(i * 30 + 1, 188, 28, 10, c);
  }
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(4, 4);
  ui->print(SONGS[danceSong].name);
  const char *name = danceMoveNames[move];
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_ORANGE);
  ui->setCursor((240 - strlen(name) * 12) / 2, 205);
  ui->print(name);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(9, 228);
  ui->printf("OK move  </> song  UP/DN tempo %d", danceBpm);
  pushUi();
}
void danceMode() {
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) danceMove = danceMove >= 3 ? -1 : danceMove + 1;
  if (pycamera.justPressed(AWEXP_BUTTON_UP)) danceBpm = min(140, danceBpm + 4);
  if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) danceBpm = max(64, danceBpm - 4);
  if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) startDance((danceSong + 1) % SONG_COUNT);
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) startDance((danceSong + SONG_COUNT - 1) % SONG_COUNT);
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 30) return;
  lastDraw = millis();
  drawDance();
}

// ===================== API STATUS (phone link) =====================
// The phone app counts the Claude requests it makes for you today and reports its battery and
// where it is. The board asks once a minute. Voice commands said into the phone app (like
// "set an alarm for 7") ride along on that reply as actions for the board.
struct PhoneInfo {
  bool valid = false; // had at least one good reply
  String error;
  unsigned long requests = 0, inputTokens = 0, outputTokens = 0;
  int battery = -1;
  bool charging = false;
  String city;
  unsigned long fetchedAt = 0;
} usage;
int usagePage = 0; // 0 = today's Claude use, 1 = the phone link
unsigned long lastUsagePoll = 0;
const unsigned long USAGE_POLL_MS = 60000;
unsigned long waveUntil = 0;
void runBoardActions(JsonArray actions, String *summary); // in CLAWD TALK
void requestUsage() {
  lastUsagePoll = millis();
  startNetJob(JOB_USAGE, "/usage", NULL, 0, "");
}
bool phoneOnline() {
  return usage.valid && usage.error.length() == 0;
}
// Why a phone request failed, in words that say what to do about it
String phoneError(int status) {
  if (status < 0) return "Can't find the Clawd Phone app on this WiFi.";
  if (status == 401) return "Not paired. In the phone app, tap Pair handheld.";
  return "Phone app error " + String(status);
}
void handleUsageResult(bool parsed, JsonDocument &doc) {
  if (!parsed) {
    usage.error = phoneError(netStatus);
    return;
  }
  if (!(doc["ok"] | false)) {
    usage.error = (const char *)(doc["error"] | "Unknown error");
    return;
  }
  unsigned long before = usage.requests;
  usage.requests = doc["today"]["requests"] | 0UL;
  usage.inputTokens = doc["today"]["input_tokens"] | 0UL;
  usage.outputTokens = doc["today"]["output_tokens"] | 0UL;
  usage.battery = doc["phone"]["battery"] | -1;
  usage.charging = doc["phone"]["charging"] | false;
  usage.city = (const char *)(doc["phone"]["city"] | "");
  if (usage.valid && usage.requests != before) waveUntil = millis() + 1500;
  usage.valid = true;
  usage.error = "";
  usage.fetchedAt = millis();
  runBoardActions(doc["actions"].as<JsonArray>(), NULL);
}
String compactUsageCount(unsigned long value) {
  char buf[12];
  if (value >= 1000000UL) snprintf(buf, sizeof(buf), "%.1fM", value / 1000000.0f);
  else if (value >= 10000UL) snprintf(buf, sizeof(buf), "%.0fk", value / 1000.0f);
  else if (value >= 1000UL) snprintf(buf, sizeof(buf), "%.1fk", value / 1000.0f);
  else snprintf(buf, sizeof(buf), "%lu", value);
  return String(buf);
}
// Little phone with its battery level, right-aligned at x; red outline when the phone is gone
void drawPhoneChip(Adafruit_GFX *g, int right, int y) {
  bool on = phoneOnline();
  bool lost = usage.error.length() > 0; // no reply yet is neither: still looking
  String pct = on ? (usage.battery >= 0 ? String(usage.battery) + "%" : String("")) : lost ? String("off") : String("...");
  int w = 10 + pct.length() * 6;
  int x = right - w;
  uint16_t c = on ? (usage.battery >= 0 && usage.battery <= 15 ? CLAUDE_RED : CLAUDE_CREAM) : lost ? CLAUDE_RED : CLAUDE_GRAY;
  g->drawRoundRect(x, y, 7, 12, 2, c);
  if (on && usage.battery >= 0) {
    int h = max(1, 8 * usage.battery / 100);
    g->fillRect(x + 2, y + 10 - h, 3, h, usage.charging ? CLAUDE_GREEN : c);
  }
  g->setTextSize(1);
  g->setTextColor(c);
  g->setCursor(x + 10, y + 3);
  g->print(pct);
}
void drawUsageHeader(const char *title) {
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print(title);
  drawPhoneChip(ui, 232, 12);
}
void drawUsageFooter(unsigned long now) {
  ui->fillCircle(10, 229, 3, phoneOnline() ? CLAUDE_GREEN : CLAUDE_RED);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(18, 226);
  if (netJob == JOB_USAGE) {
    ui->print("Refreshing...");
  } else if (!usage.valid) {
    ui->print(usage.error.length() ? "Offline" : "Connecting...");
  } else {
    unsigned long s = (now - usage.fetchedAt) / 1000;
    if (s < 60) ui->printf("Updated %lus ago", s);
    else ui->printf("Updated %lum ago", s / 60);
  }
  ui->setCursor(128, 226);
  ui->print("<> page  OK reload");
}
void drawUsageMain(unsigned long now) {
  drawUsageHeader("API Status");
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(14, 42);
  ui->print("CLAUDE REQUESTS TODAY");
  ui->setTextSize(4);
  ui->setTextColor(CLAUDE_ORANGE);
  ui->setCursor(12, 57);
  ui->print(compactUsageCount(usage.requests));
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(17, 99);
  ui->print("asks, photos, chats & translations");
  ui->drawRoundRect(12, 119, 104, 34, 7, CLAUDE_TRACK);
  ui->drawRoundRect(124, 119, 104, 34, 7, CLAUDE_TRACK);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(20, 125);
  ui->print("TOKENS IN");
  ui->setCursor(132, 125);
  ui->print("TOKENS OUT");
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(20, 137);
  ui->print(compactUsageCount(usage.inputTokens));
  ui->setCursor(132, 137);
  ui->print(compactUsageCount(usage.outputTokens));
  Face face = !phoneOnline() ? FACE_WORRIED : FACE_HAPPY;
  if (now % 4000 < 150) face = FACE_BLINK;
  int arm = now < waveUntil ? ((now / 180) % 2 ? 2 : 1) : 0;
  drawBot(ui, 20, 180, 6, face, arm, 0);
  drawBubble(ui, 76, 164, 156, 46, 66, 192);
  String quip = usage.error.length() ? usage.error
                : usage.valid        ? String("Counted on your phone. Billed to your API key, not a Claude plan.")
                                     : String("Looking for the phone app...");
  String lines[3];
  int n = wrapText(quip, 23, lines, 3);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_DARK);
  for (int i = 0; i < n; i++) {
    ui->setCursor(85, 171 + i * 12);
    ui->print(lines[i]);
  }
  drawUsageFooter(now);
}
void drawStatusRow(int y, const char *label, const String &value, uint16_t color) {
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(14, y);
  ui->print(label);
  String v = value.length() > 24 ? value.substring(0, 23) + "." : value;
  ui->setTextColor(color);
  ui->setCursor(226 - v.length() * 6, y);
  ui->print(v);
  ui->drawFastHLine(14, y + 14, 212, CLAUDE_TRACK);
}
extern bool locationKnown;
extern String myCity, locationSource;
void drawUsagePhone(unsigned long now) {
  drawUsageHeader("Phone Link");
  drawStatusRow(46, "Status", phoneOnline() ? String("Connected") : usage.valid ? String("Lost") : String("Not found"),
                phoneOnline() ? CLAUDE_GREEN : CLAUDE_RED);
  drawStatusRow(70, "Address", bridgeUrl.length() ? bridgeUrl.substring(7) : String("searching..."), CLAUDE_CREAM);
  drawStatusRow(94, "Paired", bridgeToken.length() ? String("yes") : String("tap Pair handheld in app"),
                bridgeToken.length() ? CLAUDE_CREAM : CLAUDE_AMBER);
  drawStatusRow(118, "Battery", usage.battery >= 0 ? String(usage.battery) + "%" + (usage.charging ? " charging" : "") : String("-"),
                CLAUDE_CREAM);
  drawStatusRow(142, "Location", locationKnown ? (myCity.length() ? myCity : String("known")) : String("unknown"), CLAUDE_CREAM);
  drawStatusRow(166, "From", locationKnown ? locationSource : String("-"), CLAUDE_CREAM);
  if (usage.error.length()) {
    String lines[2];
    int n = wrapText(usage.error, 36, lines, 2);
    ui->setTextColor(CLAUDE_AMBER);
    for (int i = 0; i < n; i++) {
      ui->setCursor(14, 192 + i * 11);
      ui->print(lines[i]);
    }
  }
  drawUsageFooter(now);
}
void usageMode() {
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
    requestUsage();
    pycamera.speaker_tone(1200, 20);
  }
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT) || pycamera.justPressed(AWEXP_BUTTON_RIGHT)) usagePage ^= 1;
  if (pycamera.justPressed(SHUTTER_BUTTON)) playBotHi(); // say hi again
  static unsigned long lastDraw = 0;
  unsigned long now = millis();
  if (now - lastDraw < 66) return; // ~15 fps is plenty
  lastDraw = now;
  if (usagePage == 0) drawUsageMain(now);
  else drawUsagePhone(now);
  pushUi();
}

// ===================== CLAUDE ASK =====================
// Point the camera at something and press Shutter (or OK): Claude says what it is, or solves it.
// LEFT/RIGHT on the camera view switches to Translate: signs, menus and labels in your language.
#define ASK_MAX_LINES 30
// Languages for Translate here and in Clawd Talk (sent to the phone by name)
const char *LANGS[] = {"English", "Hindi", "Spanish", "French", "German", "Japanese", "Chinese",
                       "Arabic", "Bengali", "Tamil", "Telugu", "Marathi", "Italian", "Korean", "Russian"};
#define LANG_COUNT (sizeof(LANGS) / sizeof(LANGS[0]))
int langSel = 0; // the language Translate turns things into (saved)
bool askTranslate = false;
AskState askState = ASK_PREVIEW;
bool askFailed = false;
bool askNeedsDraw = true;
String askLines[ASK_MAX_LINES];
int askLineCount = 0, askScroll = 0, askTextSize = 2;
int askReveal = 0;          // typewriter effect: characters shown so far
unsigned long askStartedAt = 0;
const char *thinkingQuips[] = {"Pondering", "Reading pixels", "Crunching maths", "Connecting dots", "Asking the spark", "Almost there"};
void showAskResult(const String &answer, bool failed) {
  askFailed = failed;
  askTextSize = 2;
  askLineCount = wrapText(answer, 16, askLines, ASK_MAX_LINES);
  if (askLineCount > 7) { // too long for big text
    askTextSize = 1;
    askLineCount = wrapText(answer, 34, askLines, ASK_MAX_LINES);
  }
  askScroll = 0;
  askReveal = 0;
  askNeedsDraw = true;
  askState = ASK_ANSWER;
  if (danceOn) return; // the dance owns the speaker
  if (failed) {
    pycamera.speaker_tone(300, 150);
  } else {
    pycamera.speaker_tone(1046, 60);
    pycamera.speaker_tone(1568, 90);
  }
}
void handleAskResult(bool parsed, JsonDocument &doc) {
  if (!parsed) {
    showAskResult(phoneError(netStatus), true);
    return;
  }
  showAskResult((const char *)(doc["answer"] | ""), !(doc["ok"] | false));
}
void handleFaceResult(bool parsed, JsonDocument &doc);
void handleWebResult(int status, const String &body);
void handleQrResult(bool parsed, JsonDocument &doc);
void handleListenResult(bool parsed, JsonDocument &doc);
void handleTalkResult(bool parsed, JsonDocument &doc);
void handleNowPlayingResult(bool parsed, JsonDocument &doc);
void handleBrowseResult(bool parsed, JsonDocument &doc);
void handleHuntResult(bool parsed, JsonDocument &doc);
void handleHuntQr(bool parsed, JsonDocument &doc);
void handleListen2Result(bool parsed, JsonDocument &doc);
void handleArtResult();
extern bool huntScanning;
extern int huntState;
extern volatile bool musicPlaying;
// Called every loop(): hands a finished network job's result to its screen
void serviceNetJob() {
  if (bridgeMoved) { // remember where the phone was, for a quicker start next boot
    bridgeMoved = false;
    prefs.putString("bridge", bridgeUrl);
  }
  if (bridgeTokenChanged) { // and the pairing token, so it only has to pair once
    bridgeTokenChanged = false;
    prefs.putString("token", bridgeToken);
  }
  if (!netDone) return;
  NetJob job = netJob;
  netDone = false;
  if (netBody) {
    free(netBody);
    netBody = NULL;
  }
  if (job == JOB_ART) {
    handleArtResult();
  } else if (job == JOB_WEB || job == JOB_LOCATION) {
    handleWebResult(netStatus, netResponse);
  } else {
    JsonDocument doc;
    bool parsed = netStatus == 200 && !deserializeJson(doc, netResponse);
    if (job == JOB_USAGE) handleUsageResult(parsed, doc);
    else if (job == JOB_FACE) handleFaceResult(parsed, doc);
    else if (job == JOB_QR && huntScanning) handleHuntQr(parsed, doc);
    else if (job == JOB_QR) handleQrResult(parsed, doc);
    else if (job == JOB_NOWPLAY) handleNowPlayingResult(parsed, doc);
    else if (job == JOB_BROWSE) handleBrowseResult(parsed, doc);
    else if (job == JOB_HUNT) handleHuntResult(parsed, doc);
    else if (job == JOB_LISTEN2) handleListen2Result(parsed, doc);
    else if (job == JOB_MEDIA) {} // nothing to show
    else if (job == JOB_LISTEN) handleListenResult(parsed, doc);
    else if (job == JOB_TALK) handleTalkResult(parsed, doc);
    else handleAskResult(parsed, doc);
  }
  netResponse = "";
  netJob = JOB_NONE;
}
bool waitNetIdle(unsigned long ms) {
  unsigned long start = millis();
  while (netBusy()) {
    serviceNetJob();
    if (millis() - start > ms) return false;
    delay(10);
  }
  return true;
}
// Takes one 1024x768 JPEG (sharp enough to read handwriting) and copies it to PSRAM
bool captureHiResJpeg(uint8_t **out, size_t *len) {
  *out = NULL;
  if (!pycamera.setFramesize(FRAMESIZE_XGA)) return false;
  for (int i = 0; i < 2; i++) { // the first frames after a resize are stale
    camera_fb_t *f = esp_camera_fb_get();
    if (f) esp_camera_fb_return(f);
  }
  camera_fb_t *f = esp_camera_fb_get();
  if (f) {
    *out = (uint8_t *)ps_malloc(f->len);
    if (*out) {
      memcpy(*out, f->buf, f->len);
      *len = f->len;
    }
    esp_camera_fb_return(f);
  }
  pycamera.setFramesize(FRAMESIZE_240X240);
  return *out != NULL;
}
void askWithPhoto() {
  pycamera.fillScreen(CLAUDE_CREAM); // camera "flash"
  pycamera.speaker_tone(1800, 30);
  uint8_t *jpg;
  size_t len = 0;
  if (!captureHiResJpeg(&jpg, &len)) {
    showAskResult("Camera capture failed.", true);
    return;
  }
  String path = askTranslate ? "/ask/image?mode=translate&lang=" + String(LANGS[langSel]) : String("/ask/image?mode=look");
  if (!waitNetIdle(8000) || !startNetJob(JOB_ASK_IMAGE, path, jpg, len, "image/jpeg")) {
    free(jpg);
    showAskResult(WiFi.status() != WL_CONNECTED ? "No WiFi connection." : "The phone link is busy. Try again.", true);
    return;
  }
  askStartedAt = millis();
  askState = ASK_THINKING;
  petBoost(3);
}
void askPreview() {
  pycamera.captureFrame();
  PyCameraFB *fb = pycamera.fb;
  fb->fillRect(0, 0, 240, 22, CLAUDE_DARK);
  drawClaudeLogo(fb, 12, 11, 7, 0, CLAUDE_ORANGE);
  fb->setTextSize(1);
  fb->setTextColor(CLAUDE_CREAM);
  fb->setCursor(24, 7);
  fb->print("Claude Ask");
  // Lens switch: Look (what is it / solve it) or Translate (into the chosen language)
  const char *lens[2] = {"LOOK", "TRANSLATE"};
  for (int i = 0, x = 106; i < 2; i++) {
    int w = strlen(lens[i]) * 6 + 10;
    bool on = askTranslate == (i == 1);
    if (on) fb->fillRoundRect(x, 4, w, 14, 7, CLAUDE_ORANGE);
    else fb->drawRoundRect(x, 4, w, 14, 7, CLAUDE_TRACK);
    fb->setTextColor(on ? CLAUDE_DARK : CLAUDE_GRAY);
    fb->setCursor(x + 5, 7);
    fb->print(lens[i]);
    x += w + 4;
  }
  fb->fillRect(0, 218, 240, 22, CLAUDE_DARK);
  fb->setTextColor(CLAUDE_CREAM);
  String hint = askTranslate ? "[Shutter] to " + String(LANGS[langSel]) + "   ^v language" : String("[Shutter] ask   </> translate");
  fb->setCursor(120 - hint.length() * 3, 226);
  fb->print(hint);
  pycamera.blitFrame();
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT) || pycamera.justPressed(AWEXP_BUTTON_RIGHT)) askTranslate = !askTranslate;
  if (askTranslate && (pycamera.justPressed(AWEXP_BUTTON_UP) || pycamera.justPressed(AWEXP_BUTTON_DOWN))) {
    langSel = (langSel + (pycamera.justPressed(AWEXP_BUTTON_UP) ? LANG_COUNT - 1 : 1)) % LANG_COUNT;
    prefs.putUChar("lang", langSel);
  }
  if (pycamera.justPressed(SHUTTER_BUTTON) || pycamera.justPressed(AWEXP_BUTTON_OK)) askWithPhoto();
}
void drawThinking() {
  unsigned long t = millis() - askStartedAt;
  ui->fillScreen(CLAUDE_DARK);
  // spark spins and pulses above the bot's head
  float pulse = 1.0f + 0.12f * sinf(t / 180.0f);
  drawClaudeLogo(ui, 120, 58, 26 * pulse, t * 0.25f, CLAUDE_ORANGE);
  // the bot glances left and right while it thinks
  int phase = (t / 700) % 4;
  Face face = phase == 0 ? FACE_LOOK_LEFT : phase == 2 ? FACE_LOOK_RIGHT : FACE_NORMAL;
  if (t % 2600 < 140) face = FACE_BLINK;
  int bob = (int)(3 * sinf(t / 250.0f));
  drawBot(ui, (240 - 7 * BOT_U) / 2, 108 + bob, BOT_U, face, 0, 0);
  const char *label = t < 3000 ? "Looking" : thinkingQuips[(t / 2200) % 6];
  int len = strlen(label) + 3; // fixed width so the dots don't shift the text
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor((240 - len * 12) / 2, 196);
  ui->print(label);
  for (int i = 0; i < (int)((t / 350) % 4); i++) ui->print('.');
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(108, 224);
  ui->printf("%lus", t / 1000);
  pushUi();
}
void drawAnswer() {
  ui->fillScreen(CLAUDE_DARK);
  drawBot(ui, 14, 10, 5, askFailed ? FACE_DEAD : FACE_HAPPY, askReveal < 9999 ? ((millis() / 180) % 2 ? 2 : 1) : 0, 0);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(66, 16);
  ui->print(askFailed ? "Something went wrong" : askTranslate ? "In " + String(LANGS[langSel]) : String("About your photo"));
  // answer bubble, revealed like it's being typed
  drawBubble(ui, 8, 46, 224, 162, 30, 38);
  int lineH = askTextSize == 2 ? 19 : 11;
  int visible = askTextSize == 2 ? 7 : 12;
  ui->setTextSize(askTextSize);
  ui->setTextColor(askFailed ? CLAUDE_RED : CLAUDE_DARK);
  int budget = askReveal;
  for (int i = askScroll; i < askLineCount && i < askScroll + visible && budget > 0; i++) {
    ui->setCursor(18, 56 + (i - askScroll) * lineH);
    String &line = askLines[i];
    ui->print(budget >= (int)line.length() ? line : line.substring(0, budget));
    budget -= line.length();
  }
  if (askScroll > 0) ui->fillTriangle(220, 54, 214, 62, 226, 62, CLAUDE_ORANGE);
  if (askScroll + visible < askLineCount) ui->fillTriangle(220, 200, 214, 192, 226, 192, CLAUDE_ORANGE);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(8, 224);
  ui->print("Shutter: new photo");
  ui->setCursor(166, 224);
  ui->print("UP/DN scroll");
  pushUi();
}
void askAnswerScreen() {
  int visible = askTextSize == 2 ? 7 : 12;
  if (pycamera.justPressed(AWEXP_BUTTON_DOWN) && askScroll + visible < askLineCount) {
    askScroll++;
    askReveal = 9999;
    askNeedsDraw = true;
  }
  if (pycamera.justPressed(AWEXP_BUTTON_UP) && askScroll > 0) {
    askScroll--;
    askNeedsDraw = true;
  }
  if (pycamera.justPressed(SHUTTER_BUTTON) || pycamera.justPressed(AWEXP_BUTTON_OK) ||
      pycamera.justPressed(AWEXP_BUTTON_LEFT)) {
    askState = ASK_PREVIEW;
    return;
  }
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 33) return;
  lastDraw = millis();
  if (askReveal < 9999) { // typewriter: ~4 characters per frame
    int total = 0;
    for (int i = 0; i < askLineCount; i++) total += askLines[i].length();
    askReveal = askReveal + 4 >= total ? 9999 : askReveal + 4;
    askNeedsDraw = true;
  }
  if (askNeedsDraw) {
    drawAnswer();
    askNeedsDraw = false;
  }
}
void askMode() {
  switch (askState) {
    case ASK_PREVIEW: askPreview(); break;
    case ASK_THINKING: drawThinking(); break;
    case ASK_ANSWER: askAnswerScreen(); break;
  }
}
// ===================== TIME KEEPING =====================
// Once the board knows the time it keeps counting by itself, WiFi or not. The time is also
// saved every few minutes, so after a reboot with no WiFi the clock, alarms and reminders
// carry on from the last saved time (shown with a "~" until internet time is back).
volatile bool timeKnown = false;  // we have a usable clock (internet or restored)
volatile bool timeApprox = false; // restored from storage, not yet confirmed online
unsigned long lastTimeSave = 0;
unsigned long lastActivity = 0;   // last button press, for the idle clock
void onTimeSync(struct timeval *tv) {
  timeKnown = true;
  timeApprox = false;
}
void saveClock() {
  lastTimeSave = millis();
  if (timeKnown) prefs.putLong64("epoch", (int64_t)time(nullptr));
}
void restoreClock() {
  int64_t saved = prefs.getLong64("epoch", 0);
  if (saved < 1700000000) return; // never saved
  struct timeval tv = {(time_t)saved, 0};
  settimeofday(&tv, NULL);
  timeKnown = true;
  timeApprox = true;
}
bool localNow(struct tm *t) {
  return timeKnown && getLocalTime(t, 0);
}
// Called every loop(): notices internet time, saves the clock, nudges WiFi to reconnect
void keepTime() {
  if (timeApprox && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) timeApprox = false;
  if (!timeKnown) {
    struct tm t;
    if (getLocalTime(&t, 0)) timeKnown = true;
  }
  if (timeKnown && millis() - lastTimeSave > 5UL * 60 * 1000) saveClock();
  static unsigned long lastWifiTry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 30000) {
    lastWifiTry = millis();
    WiFi.reconnect();
  }
}
// True on the press, then again every 70 ms while the button is held
bool pressedOrRepeat(uint8_t pin) {
  static unsigned long heldSince[16], lastRepeat[16];
  if (pycamera.justPressed(pin)) {
    heldSince[pin] = lastRepeat[pin] = millis();
    return true;
  }
  if (pycamera.button_state & (1UL << pin)) { // released
    heldSince[pin] = 0;
    return false;
  }
  if (heldSince[pin] && millis() - heldSince[pin] > 400 && millis() - lastRepeat[pin] > 70) {
    lastRepeat[pin] = millis();
    return true;
  }
  return false;
}

// ===================== ALARMS =====================
// Set up to 4 alarms with the buttons. When one goes off, Clawd dances to a funk tune
// until you press OK (stop) or any other button / shake the board (snooze 5 minutes).
#define MAX_ALARMS 4
const char *REPEAT_NAMES[] = {"Once", "Every day", "Weekdays", "Weekends"};
const uint8_t REPEAT_DAYS[] = {0, 0x7F, 0x3E, 0x41}; // bit 0 = Sunday; 0 = next time only
Alarm alarms[MAX_ALARMS] = {{false, 7, 0, 2}, {false, 8, 30, 1}, {false, 13, 0, 1}, {false, 22, 30, 1}};
int alarmSel = 0;    // row picked in the list
int alarmField = -1; // -1 = list, else editing: 0 hour, 1 minute, 2 repeat
Alarm alarmDraft;
time_t snoozeUntil = 0;
int lastAlarmMinute = -1;
Mode modeBeforeAlarm = MODE_USAGE;
unsigned long alarmRangAt = 0;
void saveAlarms() {
  prefs.putBytes("alarms", alarms, sizeof(alarms));
}
void loadAlarms() {
  if (prefs.getBytesLength("alarms") == sizeof(alarms)) prefs.getBytes("alarms", alarms, sizeof(alarms));
}
// Minutes until this alarm next rings (-1 if it's off)
int minutesUntil(const Alarm &a, const struct tm &now) {
  if (!a.on) return -1;
  int nowMin = now.tm_hour * 60 + now.tm_min, at = a.hour * 60 + a.minute;
  uint8_t days = REPEAT_DAYS[a.repeat];
  for (int d = 0; d < 8; d++) {
    if (d == 0 && at <= nowMin) continue;
    if (days == 0 || (days & (1 << ((now.tm_wday + d) % 7)))) return d * 1440 + at - nowMin;
  }
  return -1;
}
// Index of the alarm that rings soonest (-1 if none), and how many minutes away
int nextAlarm(int *mins) {
  struct tm now;
  if (!localNow(&now)) return -1;
  int best = -1, bestMins = 0;
  for (int i = 0; i < MAX_ALARMS; i++) {
    int m = minutesUntil(alarms[i], now);
    if (m >= 0 && (best < 0 || m < bestMins)) {
      best = i;
      bestMins = m;
    }
  }
  *mins = bestMins;
  return best;
}
String clockText(int hour, int minute) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, hour < 12 ? "AM" : "PM");
  return buf;
}
String waitText(int mins) {
  char buf[16];
  if (mins >= 1440) snprintf(buf, sizeof(buf), "%dd %dh", mins / 1440, mins % 1440 / 60);
  else if (mins >= 60) snprintf(buf, sizeof(buf), "%dh %dm", mins / 60, mins % 60);
  else snprintf(buf, sizeof(buf), "%dm", mins);
  return buf;
}
// Called every loop(): rings an alarm (or a snooze) when its minute arrives
void checkAlarms() {
  struct tm t;
  if (!localNow(&t)) return;
  int key = t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;
  if (key == lastAlarmMinute) return;
  lastAlarmMinute = key;
  if (snoozeUntil && time(nullptr) >= snoozeUntil) {
    snoozeUntil = 0;
    ringAlarm();
    return;
  }
  for (int i = 0; i < MAX_ALARMS; i++) {
    Alarm &a = alarms[i];
    if (!a.on || a.hour != t.tm_hour || a.minute != t.tm_min) continue;
    uint8_t days = REPEAT_DAYS[a.repeat];
    if (days && !(days & (1 << t.tm_wday))) continue;
    if (!days) { // one-time alarm: switch it off now that it has rung
      a.on = false;
      saveAlarms();
    }
    ringAlarm();
    return;
  }
}
void ringAlarm() {
  if (currentMode == MODE_ALARM_RING) return;
  musicStop();
  if (currentMode == MODE_SLEEP) wakeUp();
  stopDance();
  modeBeforeAlarm = currentMode == MODE_DANCE ? MODE_USAGE : currentMode;
  currentMode = MODE_ALARM_RING;
  alarmRangAt = millis();
  startDance(SONG_ALARM);
}
void endAlarm(bool snooze) {
  stopDance();
  snoozeUntil = snooze && timeKnown ? time(nullptr) + 5 * 60 : 0;
  currentMode = modeBeforeAlarm;
  lastActivity = millis();
  askNeedsDraw = true;
  if (currentMode == MODE_MENU) drawMenu();
}
// Acceleration in g (1.0 = gravity). The PyCamera library reports about 9.8 at rest
// (m/s^2), so the scale is measured once at startup by calibrateAccel().
float accelScale = 9.81f;
void calibrateAccel() {
  float x, y, z;
  if (pycamera.readAccelData(&x, &y, &z)) accelScale = sqrtf(x * x + y * y + z * z) > 4 ? 9.81f : 1.0f;
}
bool readAccelG(float *g) {
  float x, y, z;
  if (!pycamera.readAccelData(&x, &y, &z)) return false;
  g[0] = x / accelScale;
  g[1] = y / accelScale;
  g[2] = z / accelScale;
  return true;
}
bool shaken() {
  float g[3];
  if (!readAccelG(g)) return false;
  return fabsf(sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) - 1.0f) > 0.9f;
}
void drawAlarmRing() {
  int step;
  float phase;
  danceClock(step, phase);
  unsigned long now = millis();
  bool kick = SONGS[danceSong].drums[step % 16] == 'K';
  ui->fillScreen((step / 4) % 2 ? 0x38E3 : CLAUDE_DARK); // background pulses on the beat
  drawClaudeLogo(ui, 120, 30, 20 + (kick ? 5 * (1 - phase) : 0), step * 20 + phase * 20, CLAUDE_ORANGE);
  struct tm t;
  if (localNow(&t)) {
    String c = clockText(t.tm_hour, t.tm_min);
    ui->setTextSize(3);
    ui->setTextColor(CLAUDE_CREAM);
    ui->setCursor(120 - c.length() * 9, 60);
    ui->print(c);
  }
  int dx, dy, leftArm, rightArm;
  Face face;
  dancePose(step, phase, (step / 16) % 4, dx, dy, leftArm, rightArm, face);
  drawBot(ui, 78 + dx, 104 + dy, 12, face, rightArm, leftArm);
  const char *msg = "WAKE UP!";
  ui->setTextSize(3);
  ui->setTextColor(CLAUDE_ORANGE);
  for (int i = 0; i < 8; i++) { // bouncing letters
    ui->setCursor(48 + i * 18, 182 + (int)(5 * sinf(now / 120.0f + i * 0.8f)));
    ui->print(msg[i]);
  }
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(12, 222);
  ui->print("OK: stop   any key/shake: snooze 5m");
  pushUi();
}
void alarmRingMode() {
  if (pycamera.justPressed(AWEXP_BUTTON_OK) || pycamera.justPressed(SHUTTER_BUTTON)) {
    endAlarm(false);
    return;
  }
  if (pycamera.justPressed(AWEXP_BUTTON_UP) || pycamera.justPressed(AWEXP_BUTTON_DOWN) ||
      pycamera.justPressed(AWEXP_BUTTON_LEFT) || pycamera.justPressed(AWEXP_BUTTON_RIGHT) || shaken()) {
    endAlarm(true);
    return;
  }
  if (millis() - alarmRangAt > 3UL * 60 * 1000) { // give up after 3 minutes
    endAlarm(false);
    return;
  }
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 30) return;
  lastDraw = millis();
  drawAlarmRing();
}
void drawAlarmsList() {
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print("Alarms");
  struct tm now;
  bool clock = localNow(&now);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  String c = clock ? (timeApprox ? "~" : "") + clockText(now.tm_hour, now.tm_min) : String("--:--");
  ui->setCursor(232 - c.length() * 6, 15);
  ui->print(c);
  for (int i = 0; i < MAX_ALARMS; i++) {
    Alarm &a = alarms[i];
    int y = 38 + i * 40;
    bool sel = i == alarmSel;
    if (sel) ui->fillRoundRect(8, y, 224, 36, 8, 0x2104);
    ui->drawRoundRect(8, y, 224, 36, 8, sel ? CLAUDE_ORANGE : CLAUDE_TRACK);
    ui->setTextSize(2);
    ui->setTextColor(a.on ? CLAUDE_CREAM : CLAUDE_GRAY);
    ui->setCursor(18, y + 5);
    ui->print(clockText(a.hour, a.minute));
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(18, y + 24);
    ui->print(REPEAT_NAMES[a.repeat]);
    // on/off switch
    ui->fillRoundRect(180, y + 10, 40, 16, 8, a.on ? CLAUDE_ORANGE : CLAUDE_TRACK);
    ui->fillCircle(a.on ? 211 : 188, y + 18, 6, a.on ? CLAUDE_CREAM : CLAUDE_GRAY);
  }
  ui->setTextSize(1);
  int mins;
  int next = nextAlarm(&mins);
  String status = !clock      ? String("Clock not set - connect WiFi once")
                  : next >= 0 ? "Next: " + clockText(alarms[next].hour, alarms[next].minute) + ", in " + waitText(mins)
                              : String("No alarms on");
  if (snoozeUntil) status = "Snoozing... rings again in 5 min";
  ui->setTextColor(next >= 0 || snoozeUntil ? CLAUDE_ORANGE : CLAUDE_GRAY);
  ui->setCursor(120 - status.length() * 3, 200);
  ui->print(status);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(21, 222);
  ui->print("UP/DN pick  OK edit  RIGHT on/off");
  pushUi();
}
void drawAlarmEdit() {
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->printf("Alarm %d", alarmSel + 1);
  // big time; the field being edited is orange with arrows above and below
  int h12 = alarmDraft.hour % 12 ? alarmDraft.hour % 12 : 12;
  char hh[4], mm[4];
  snprintf(hh, sizeof(hh), "%2d", h12);
  snprintf(mm, sizeof(mm), "%02d", alarmDraft.minute);
  const int x0 = 18, y0 = 62;
  ui->setTextSize(6);
  ui->setTextColor(alarmField == 0 ? CLAUDE_ORANGE : CLAUDE_CREAM);
  ui->setCursor(x0, y0);
  ui->print(hh);
  ui->setTextColor(CLAUDE_CREAM);
  ui->print(':');
  ui->setTextColor(alarmField == 1 ? CLAUDE_ORANGE : CLAUDE_CREAM);
  ui->print(mm);
  ui->setTextSize(3);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(x0 + 5 * 36 + 4, y0 + 24);
  ui->print(alarmDraft.hour < 12 ? "AM" : "PM");
  if (alarmField <= 1) {
    int fx = x0 + (alarmField == 0 ? 0 : 3 * 36) + 36; // centre of the two digits
    ui->fillTriangle(fx, y0 - 14, fx - 8, y0 - 6, fx + 8, y0 - 6, CLAUDE_ORANGE);
    ui->fillTriangle(fx, y0 + 64, fx - 8, y0 + 56, fx + 8, y0 + 56, CLAUDE_ORANGE);
  }
  // repeat
  String rep = String("< ") + REPEAT_NAMES[alarmDraft.repeat] + " >";
  ui->drawRoundRect(30, 136, 180, 28, 8, alarmField == 2 ? CLAUDE_ORANGE : CLAUDE_TRACK);
  drawCentered(rep, 142, 2, alarmField == 2 ? CLAUDE_ORANGE : CLAUDE_CREAM);
  // which days that means
  const char *D = "SMTWTFS";
  uint8_t days = REPEAT_DAYS[alarmDraft.repeat];
  ui->setTextSize(2);
  for (int i = 0; i < 7; i++) {
    ui->setTextColor(days & (1 << i) ? CLAUDE_ORANGE : CLAUDE_TRACK);
    ui->setCursor(36 + i * 25, 178);
    ui->print(D[i]);
  }
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(6, 222);
  ui->print("</> field  UP/DN change  OK save");
  pushUi();
}
void alarmsMode() {
  if (alarmField < 0) {
    if (pycamera.justPressed(AWEXP_BUTTON_UP)) alarmSel = (alarmSel + MAX_ALARMS - 1) % MAX_ALARMS;
    if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) alarmSel = (alarmSel + 1) % MAX_ALARMS;
    if (pycamera.justPressed(AWEXP_BUTTON_RIGHT) || pycamera.justPressed(AWEXP_BUTTON_LEFT)) {
      alarms[alarmSel].on = !alarms[alarmSel].on;
      saveAlarms();
      pycamera.speaker_tone(alarms[alarmSel].on ? 1320 : 660, 30);
    }
    if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
      alarmDraft = alarms[alarmSel];
      alarmField = 0;
    }
  } else {
    if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) alarmField = (alarmField + 2) % 3;
    if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) alarmField = (alarmField + 1) % 3;
    int delta = pressedOrRepeat(AWEXP_BUTTON_UP) ? 1 : pressedOrRepeat(AWEXP_BUTTON_DOWN) ? -1 : 0;
    if (delta) {
      if (alarmField == 0) alarmDraft.hour = (alarmDraft.hour + 24 + delta) % 24;
      else if (alarmField == 1) alarmDraft.minute = (alarmDraft.minute + 60 + delta) % 60;
      else alarmDraft.repeat = (alarmDraft.repeat + 4 + delta) % 4;
    }
    if (pycamera.justPressed(AWEXP_BUTTON_OK)) { // save and switch it on
      alarmDraft.on = true;
      alarms[alarmSel] = alarmDraft;
      saveAlarms();
      alarmField = -1;
      pycamera.speaker_tone(1046, 40);
      pycamera.speaker_tone(1568, 60);
    }
    if (pycamera.justPressed(SHUTTER_BUTTON)) alarmField = -1; // cancel
  }
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 50) return;
  lastDraw = millis();
  if (alarmField < 0) drawAlarmsList();
  else drawAlarmEdit();
}

// ===================== FACE FINDER =====================
// The camera streams its small preview frames to the phone app, which finds faces with
// ML Kit on the phone. (This board's ESP32 core no longer ships Espressif's face detector.)
struct FaceBox {
  int16_t x, y, w, h;
};
#define MAX_FACES 6
FaceBox faces[MAX_FACES];
int faceCount = 0;
unsigned long facesAt = 0, lastFaceSend = 0, lastFaceSeen = 0, hiUntil = 0;
bool faceBridgeDown = false;
void handleFaceResult(bool parsed, JsonDocument &doc) {
  faceBridgeDown = !parsed;
  if (!parsed) return;
  int n = 0;
  for (JsonArray f : doc["faces"].as<JsonArray>()) {
    if (n >= MAX_FACES) break;
    faces[n++] = {(int16_t)(f[0] | 0), (int16_t)(f[1] | 0), (int16_t)(f[2] | 0), (int16_t)(f[3] | 0)};
  }
  faceCount = n;
  facesAt = millis();
  if (n > 0) {
    if (millis() - lastFaceSeen > 3000) { // someone new showed up: say hi
      hiUntil = millis() + 1800;
      if (!danceOn) pycamera.speaker_tone(1320, 25);
    }
    lastFaceSeen = millis();
  }
}
void faceMode() {
  pycamera.captureFrame();
  // Send the frame we just took to the bridge whenever it's free (a few times a second)
  if (!netBusy() && pycamera.frame && millis() - lastFaceSend > 150) {
    size_t len = pycamera.frame->len;
    uint8_t *jpg = (uint8_t *)ps_malloc(len);
    if (jpg) {
      memcpy(jpg, pycamera.frame->buf, len);
      if (startNetJob(JOB_FACE, "/face", jpg, len, "image/jpeg")) lastFaceSend = millis();
      else free(jpg);
    }
  }
  PyCameraFB *fb = pycamera.fb;
  if (millis() - facesAt > 1200) faceCount = 0; // boxes this old no longer match the picture
  for (int i = 0; i < faceCount; i++) {
    FaceBox &f = faces[i];
    fb->drawRoundRect(f.x, f.y, f.w, f.h, 6, CLAUDE_ORANGE);
    fb->drawRoundRect(f.x + 1, f.y + 1, f.w - 2, f.h - 2, 5, CLAUDE_ORANGE);
    int ly = max(20, f.y - 13);
    fb->fillRoundRect(f.x, ly, 50, 12, 3, CLAUDE_ORANGE);
    fb->setTextSize(1);
    fb->setTextColor(CLAUDE_DARK);
    fb->setCursor(f.x + 4, ly + 2);
    fb->printf("Human %d", i + 1);
  }
  fb->fillRect(0, 0, 240, 18, CLAUDE_DARK);
  drawClaudeLogo(fb, 10, 9, 6, 0, CLAUDE_ORANGE);
  fb->setTextSize(1);
  fb->setTextColor(CLAUDE_CREAM);
  fb->setCursor(20, 5);
  if (faceBridgeDown) fb->print("Face Finder - phone offline");
  else if (faceCount == 0) fb->print("Face Finder - looking...");
  else fb->printf("Face Finder - %d face%s", faceCount, faceCount > 1 ? "s" : "");
  // Clawd in the corner keeps an eye on the first face
  Face look = FACE_NORMAL;
  if (faceCount) {
    int cx = faces[0].x + faces[0].w / 2;
    look = cx < 90 ? FACE_LOOK_LEFT : cx > 150 ? FACE_LOOK_RIGHT : FACE_HAPPY;
  }
  if (millis() % 3000 < 120) look = FACE_BLINK;
  bool waving = millis() < hiUntil;
  fb->fillRoundRect(0, 200, 52, 40, 6, CLAUDE_DARK);
  drawBot(fb, 12, 210, 4, look, waving ? ((millis() / 150) % 2 ? 2 : 1) : 0, 0);
  if (waving) {
    drawBubble(fb, 54, 192, 40, 20, 46, 206);
    fb->setTextColor(CLAUDE_ORANGE);
    fb->setCursor(64, 198);
    fb->print("Hi!");
  }
  pycamera.blitFrame();
}

// ===================== WORLD LIVE + DAILY FEED =====================
// Free web data the board fetches itself over WiFi, no API keys (except YouTube's free one):
//   World Live: Open-Meteo (weather, air quality, sunrise), OpenSky (planes), wheretheiss.at
//               (ISS), NOAA SWPC (space weather), The Space Devs (rocket launches), USGS (quakes)
//   Daily Feed: YouTube Data API (your subscribers), CoinGecko (crypto), Frankfurter (ECB
//               exchange rates), Hacker News (Algolia), byabbe.se (on this day), Wikipedia,
//               Open Trivia DB and JokeAPI
// Your location comes from the phone's GPS (or, without the phone, a guess from your internet
// connection via ipwho.is) unless you pin it here.
#define MY_LAT 0.0f // e.g. 23.2599 to pin your location; 0 = automatic
#define MY_LON 0.0f
#define W_WEATHER 0
#define W_PLANES 1
#define W_ISS 2
#define W_QUAKES 3
#define W_CRYPTO 4
#define W_AIR 5
#define W_SKY 6
#define W_SPACEWX 7
#define W_LAUNCH 8
#define W_YOUTUBE 9
#define W_FX 10
#define W_NEWS 11
#define W_HISTORY 12
#define W_WIKI 13
#define W_TRIVIA 14
#define W_JOKE 15
#define W_PAGES 16
#define W_LOCATE 16 // not a page: the location lookup
#define CLAUDE_LAND 0x3A67  // #384E3C
#define CLAUDE_CLOUD 0xCE59 // #C8C8CC
const char *W_TITLES[] = {"Weather", "Planes nearby", "ISS tracker", "Earthquakes", "Crypto",
                          "Air quality", "Sun & Moon", "Space weather", "Launches", "YouTube",
                          "Currency", "Hacker News", "On this day", "Wikipedia", "Trivia", "Joke"};
#define MIN_MS 60000UL
// How often each page refreshes while it's on screen (the last six also refresh on OK)
const unsigned long W_REFRESH_MS[] = {15 * MIN_MS, MIN_MS, 10000, 10 * MIN_MS, 2 * MIN_MS,
                                      30 * MIN_MS, 360 * MIN_MS, 30 * MIN_MS, 30 * MIN_MS, 5 * MIN_MS,
                                      360 * MIN_MS, 15 * MIN_MS, 360 * MIN_MS, 360 * MIN_MS, 360 * MIN_MS, 360 * MIN_MS, 0};
// The two menus share these pages
const uint8_t WORLD_SET[] = {W_WEATHER, W_AIR, W_SKY, W_PLANES, W_ISS, W_SPACEWX, W_LAUNCH, W_QUAKES};
const uint8_t FEED_SET[] = {W_YOUTUBE, W_CRYPTO, W_FX, W_NEWS, W_HISTORY, W_WIKI, W_TRIVIA, W_JOKE};
bool feedMode = false;      // MODE_WORLD is showing Daily Feed instead of World Live
int worldIdx = 0, feedIdx = 0;
unsigned long wFetchedAt[W_PAGES + 1];
bool wHasData[W_PAGES + 1];
String wError[W_PAGES + 1];
int worldPage = 0;
bool needsLocation(int w) {
  return w == W_WEATHER || w == W_PLANES || w == W_AIR || w == W_SKY;
}
String feedUrl(int w);
void feedFilter(int w, JsonDocument &filter);
bool parseFeed(int w, JsonDocument &doc);
bool pageReady(int w);
int webWidget = -1; // which widget the running web request is for
float myLat = MY_LAT, myLon = MY_LON;
String myCity;
bool locationKnown = MY_LAT != 0.0f || MY_LON != 0.0f;
bool locationPinned = locationKnown; // MY_LAT/MY_LON set: never look it up
String locationSource = locationPinned ? "Pinned in sketch" : "";
bool locateTried = false;
unsigned long locatedAt = 0;              // when the last lookup finished (0 = never)
const unsigned long LOCATE_EVERY_MS = 15UL * 60 * 1000; // it's a pocket device: you move around
const unsigned long LOCATE_RETRY_MS = 2UL * 60 * 1000;
// True when it's time to ask the phone (or the internet) where we are
bool locationDue() {
  if (locationPinned || netBusy() || WiFi.status() != WL_CONNECTED) return false;
  if (!locateTried) return true;
  return millis() - locatedAt > (locationKnown ? LOCATE_EVERY_MS : LOCATE_RETRY_MS);
}
// Weather
struct {
  float temp, feels, wind;
  int humidity, code;
  bool isDay;
  int dCode[3];
  float dMax[3], dMin[3];
} wx;
// Planes
struct Plane {
  char call[9];
  float dist, bearing;
  int altFt, speed, track;
};
#define MAX_PLANES 24
#define PLANE_RANGE_KM 150.0f
Plane planes[MAX_PLANES];
int planeCount = 0;
// ISS
float issLat, issLon, issAlt, issSpeed;
bool issDaylight = false, issNearAlerted = false;
#define ISS_TRAIL 90
float issTrailLat[ISS_TRAIL], issTrailLon[ISS_TRAIL];
int issTrailLen = 0;
// Earthquakes
struct Quake {
  float mag, lat, lon;
  char place[40];
  int64_t timeMs;
};
#define MAX_QUAKES 12
Quake quakes[MAX_QUAKES];
int quakeCount = 0;
// Crypto (prices in INR)
const char *COIN_IDS[] = {"bitcoin", "ethereum", "solana", "dogecoin"};
const char *COIN_SYMS[] = {"BTC", "ETH", "SOL", "DOGE"};
const char *COIN_NAMES[] = {"Bitcoin", "Ethereum", "Solana", "Dogecoin"};
const uint16_t COIN_COLORS[] = {0xF483, 0x63FD, 0x9A3F, 0xC526};
#define COIN_HIST 40
float coinPrice[4], coinChange[4];
float coinHist[4][COIN_HIST];
int coinHistLen = 0;
// 80x40 land mask of the world, 4.5 degrees per cell (generated from global-land-mask)
const uint8_t WORLD_MAP[400] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x07, 0xD3, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x7F, 0xFF, 0xF0, 0x1C, 0x00, 0x05, 0x00, 0x00,
  0x00, 0x0F, 0xBE, 0x1F, 0xE0, 0x00, 0x0C, 0x3F, 0xC1, 0x00,
  0x1F, 0xFF, 0xBF, 0x8F, 0xE0, 0x0F, 0x0B, 0xFF, 0xFF, 0xFF,
  0xDF, 0xFF, 0xFD, 0xCF, 0x18, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF,
  0x1F, 0xFF, 0xE3, 0xC6, 0x00, 0x77, 0xFF, 0xFF, 0xFF, 0xFE,
  0x00, 0x3F, 0xFB, 0xE0, 0x03, 0x37, 0xFF, 0xFF, 0xFE, 0x10,
  0x00, 0x1F, 0xFF, 0xF0, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
  0x00, 0x0F, 0xFF, 0xD0, 0x01, 0xFF, 0xFF, 0xFF, 0xFE, 0x00,
  0x00, 0x0F, 0xFF, 0x80, 0x03, 0xFD, 0x7F, 0xFF, 0xFD, 0x00,
  0x00, 0x0F, 0xFE, 0x00, 0x03, 0x13, 0xFF, 0xFF, 0xE9, 0x00,
  0x00, 0x07, 0xFE, 0x00, 0x03, 0xE0, 0xFF, 0xFF, 0xE6, 0x00,
  0x00, 0x03, 0xFC, 0x00, 0x07, 0xFF, 0xFF, 0xFF, 0xE0, 0x00,
  0x00, 0x01, 0xE0, 0x00, 0x0F, 0xFF, 0xFF, 0xFF, 0xE0, 0x00,
  0x00, 0x00, 0x69, 0x00, 0x0F, 0xFF, 0xF8, 0xEF, 0x80, 0x00,
  0x00, 0x00, 0x38, 0x00, 0x0F, 0xFF, 0xF0, 0xC7, 0x20, 0x00,
  0x00, 0x00, 0x05, 0xC0, 0x0F, 0xFF, 0xE0, 0x41, 0x10, 0x00,
  0x00, 0x00, 0x01, 0xF0, 0x07, 0xFF, 0xE0, 0x02, 0x10, 0x00,
  0x00, 0x00, 0x03, 0xF8, 0x00, 0x3F, 0xC0, 0x02, 0xC0, 0x00,
  0x00, 0x00, 0x03, 0xFE, 0x00, 0x3F, 0x80, 0x03, 0xE7, 0x00,
  0x00, 0x00, 0x03, 0xFF, 0x00, 0x1F, 0x80, 0x01, 0x81, 0x80,
  0x00, 0x00, 0x01, 0xFF, 0x00, 0x1F, 0x80, 0x00, 0x04, 0x00,
  0x00, 0x00, 0x01, 0xFF, 0x00, 0x3F, 0xA0, 0x00, 0x0F, 0x00,
  0x00, 0x00, 0x00, 0xFE, 0x00, 0x1F, 0x20, 0x00, 0x3F, 0x80,
  0x00, 0x00, 0x00, 0xFC, 0x00, 0x1F, 0x20, 0x00, 0x7F, 0xC0,
  0x00, 0x00, 0x00, 0xF8, 0x00, 0x0E, 0x00, 0x00, 0x7F, 0xC0,
  0x00, 0x00, 0x00, 0xF0, 0x00, 0x0C, 0x00, 0x00, 0x33, 0xC0,
  0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x01, 0x81,
  0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82,
  0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
  0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x10, 0x00, 0x04, 0x00,
  0x00, 0x00, 0x00, 0xC0, 0x00, 0x7B, 0xFF, 0xFF, 0xFF, 0xF8,
  0x00, 0x7F, 0xFF, 0xE0, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8,
  0x07, 0xFF, 0xFE, 0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0,
  0x8F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
float distanceKm(float lat1, float lon1, float lat2, float lon2) {
  float p1 = lat1 * DEG_TO_RAD, p2 = lat2 * DEG_TO_RAD;
  float dp = p2 - p1, dl = (lon2 - lon1) * DEG_TO_RAD;
  float a = sinf(dp / 2) * sinf(dp / 2) + cosf(p1) * cosf(p2) * sinf(dl / 2) * sinf(dl / 2);
  return 6371.0f * 2 * atan2f(sqrtf(a), sqrtf(1 - a));
}
float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  float p1 = lat1 * DEG_TO_RAD, p2 = lat2 * DEG_TO_RAD, dl = (lon2 - lon1) * DEG_TO_RAD;
  float b = atan2f(sinf(dl) * cosf(p2), cosf(p1) * sinf(p2) - sinf(p1) * cosf(p2) * cosf(dl));
  return fmodf(b * RAD_TO_DEG + 360.0f, 360.0f);
}
// 1234567 -> "1,234,567", or Indian style "12,34,567"
String withCommas(long v, bool indian) {
  String digits = String(v < 0 ? -v : v), out;
  int n = digits.length();
  for (int i = 0; i < n; i++) {
    int left = n - i;
    out += digits[i];
    if (left > 1 && (indian ? (left == 4 || (left > 4 && left % 2 == 0)) : (left - 1) % 3 == 0)) out += ',';
  }
  return v < 0 ? "-" + out : out;
}
String widgetUrl(int w) {
  char url[300];
  switch (w) {
    case W_WEATHER:
      snprintf(url, sizeof(url),
               "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
               "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=4",
               myLat, myLon);
      return url;
    case W_PLANES: // OpenSky allows ~400 anonymous requests a day, so this only runs while you watch
      snprintf(url, sizeof(url), "https://opensky-network.org/api/states/all?lamin=%.2f&lomin=%.2f&lamax=%.2f&lomax=%.2f",
               myLat - 1.4f, myLon - 1.4f, myLat + 1.4f, myLon + 1.4f);
      return url;
    case W_ISS:
      return "https://api.wheretheiss.at/v1/satellites/25544";
    case W_QUAKES:
      return "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/4.5_day.geojson";
    case W_CRYPTO:
      return "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum,solana,dogecoin&vs_currencies=inr&include_24hr_change=true";
    default:
      return feedUrl(w);
  }
}
void requestWidget(int w) {
  bool started = w == W_LOCATE
                     ? startNetJob(JOB_LOCATION, "/location", NULL, 0, "")
                     : startNetJob(JOB_WEB, widgetUrl(w), NULL, 0, "");
  if (started) {
    webWidget = w;
    wFetchedAt[w] = millis();
  }
}
// Called while World Live is on screen: fetch the visible page when it's due
void worldTick() {
  if (netBusy()) return;
  int w = worldPage;
  if (locationDue()) {
    locateTried = true;
    requestWidget(W_LOCATE);
    return;
  }
  if (needsLocation(w) && !locationKnown) return;
  if (!pageReady(w)) return; // e.g. YouTube without a key
  if (!wFetchedAt[w] || millis() - wFetchedAt[w] > W_REFRESH_MS[w]) requestWidget(w);
}
void handleWebResult(int status, const String &body) {
  int w = webWidget;
  webWidget = -1;
  if (w < 0) return;
  if (w == W_LOCATE) locatedAt = millis();
  if (status != 200) {
    wError[w] = status == 429                                  ? String("Too many requests - try later")
              : status < 0                                     ? String("No internet connection")
              : w == W_YOUTUBE && (status == 400 || status == 403) ? String("YouTube refused the API key (YT_API_KEY)")
                                                               : "Server error " + String(status);
    if (w != W_LOCATE) wFetchedAt[w] = millis() - W_REFRESH_MS[w] + 20000; // retry in 20 s
    return;
  }
  JsonDocument doc;
  DeserializationError err;
  if (w == W_QUAKES) { // the USGS feed is big: keep only the fields we show
    JsonDocument filter;
    filter["features"][0]["properties"]["mag"] = true;
    filter["features"][0]["properties"]["place"] = true;
    filter["features"][0]["properties"]["time"] = true;
    filter["features"][0]["geometry"]["coordinates"] = true;
    err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  } else if (w >= W_AIR && w != W_LOCATE) { // the newer pages keep only what they show
    JsonDocument filter;
    feedFilter(w, filter);
    err = filter.isNull() ? deserializeJson(doc, body) : deserializeJson(doc, body, DeserializationOption::Filter(filter));
  } else {
    err = deserializeJson(doc, body);
  }
  if (err) {
    wError[w] = "Couldn't read the data";
    return;
  }
  wError[w] = "";
  switch (w) {
    case W_LOCATE:
      if (doc["success"] | false) {
        float lat = doc["latitude"] | 0.0f, lon = doc["longitude"] | 0.0f;
        if (locationKnown && distanceKm(myLat, myLon, lat, lon) > 3) { // moved: refresh what depends on it
          for (int i = 0; i < W_PAGES; i++)
            if (needsLocation(i)) wFetchedAt[i] = 0;
          planeCount = 0;
        }
        myLat = lat;
        myLon = lon;
        myCity = (const char *)(doc["city"] | "");
        locationSource = strcmp(doc["source"] | "", "phone") == 0 ? "Phone GPS" : "Internet (IP guess)";
        locationKnown = true;
      }
      break;
    case W_WEATHER: {
      JsonObject c = doc["current"];
      wx.temp = c["temperature_2m"] | 0.0f;
      wx.feels = c["apparent_temperature"] | 0.0f;
      wx.humidity = c["relative_humidity_2m"] | 0;
      wx.code = c["weather_code"] | 0;
      wx.wind = c["wind_speed_10m"] | 0.0f;
      wx.isDay = (c["is_day"] | 1) == 1;
      for (int i = 0; i < 3; i++) { // tomorrow onwards
        wx.dCode[i] = doc["daily"]["weather_code"][i + 1] | 0;
        wx.dMax[i] = doc["daily"]["temperature_2m_max"][i + 1] | 0.0f;
        wx.dMin[i] = doc["daily"]["temperature_2m_min"][i + 1] | 0.0f;
      }
      break;
    }
    case W_PLANES:
      planeCount = 0;
      for (JsonArray s : doc["states"].as<JsonArray>()) {
        if (planeCount >= MAX_PLANES) break;
        if (s[5].isNull() || s[6].isNull() || (s[8] | false)) continue; // no position, or on the ground
        Plane &p = planes[planeCount++];
        strlcpy(p.call, s[1] | "", sizeof(p.call));
        for (int i = strlen(p.call) - 1; i >= 0 && p.call[i] == ' '; i--) p.call[i] = 0;
        if (!p.call[0]) strlcpy(p.call, s[0] | "?", sizeof(p.call));
        float la = s[6] | 0.0f, lo = s[5] | 0.0f;
        p.dist = distanceKm(myLat, myLon, la, lo);
        p.bearing = bearingDeg(myLat, myLon, la, lo);
        float altM = s[13].isNull() ? (s[7] | 0.0f) : (s[13] | 0.0f);
        p.altFt = (int)(altM * 3.281f);
        p.speed = (int)((s[9] | 0.0f) * 3.6f);
        p.track = (int)(s[10] | 0.0f);
      }
      for (int i = 1; i < planeCount; i++) { // nearest first
        Plane p = planes[i];
        int j = i - 1;
        while (j >= 0 && planes[j].dist > p.dist) {
          planes[j + 1] = planes[j];
          j--;
        }
        planes[j + 1] = p;
      }
      break;
    case W_ISS:
      issLat = doc["latitude"] | 0.0f;
      issLon = doc["longitude"] | 0.0f;
      issAlt = doc["altitude"] | 0.0f;
      issSpeed = doc["velocity"] | 0.0f;
      issDaylight = strcmp(doc["visibility"] | "", "daylight") == 0;
      if (issTrailLen == ISS_TRAIL) {
        memmove(issTrailLat, issTrailLat + 1, sizeof(float) * (ISS_TRAIL - 1));
        memmove(issTrailLon, issTrailLon + 1, sizeof(float) * (ISS_TRAIL - 1));
        issTrailLen--;
      }
      issTrailLat[issTrailLen] = issLat;
      issTrailLon[issTrailLen++] = issLon;
      if (locationKnown) {
        float d = distanceKm(myLat, myLon, issLat, issLon);
        if (d < 1500 && !issNearAlerted) {
          issNearAlerted = true;
          for (int i = 0; i < 3; i++) pycamera.speaker_tone(1760, 60);
        }
        if (d > 2500) issNearAlerted = false;
      }
      break;
    case W_QUAKES:
      quakeCount = 0;
      for (JsonObject f : doc["features"].as<JsonArray>()) {
        if (quakeCount >= MAX_QUAKES) break;
        Quake &q = quakes[quakeCount++];
        q.mag = f["properties"]["mag"] | 0.0f;
        strlcpy(q.place, f["properties"]["place"] | "", sizeof(q.place));
        q.timeMs = f["properties"]["time"].as<long long>();
        q.lon = f["geometry"]["coordinates"][0] | 0.0f;
        q.lat = f["geometry"]["coordinates"][1] | 0.0f;
      }
      break;
    case W_CRYPTO:
      if (coinHistLen == COIN_HIST) {
        for (int c = 0; c < 4; c++) memmove(coinHist[c], coinHist[c] + 1, sizeof(float) * (COIN_HIST - 1));
        coinHistLen--;
      }
      for (int c = 0; c < 4; c++) {
        coinPrice[c] = doc[COIN_IDS[c]]["inr"] | 0.0f;
        coinChange[c] = doc[COIN_IDS[c]]["inr_24h_change"] | 0.0f;
        coinHist[c][coinHistLen] = coinPrice[c];
      }
      coinHistLen++;
      break;
    default:
      if (!parseFeed(w, doc)) return; // parseFeed set wError
      break;
  }
  wHasData[w] = true;
}
int mapX(float lon) {
  return (int)((lon + 180.0f) / 360.0f * 240.0f);
}
int mapY(float lat, int y0) {
  return y0 + (int)((90.0f - lat) / 180.0f * 120.0f);
}
void drawWorldMap(int y0) {
  for (int r = 0; r < 40; r++) {
    for (int c = 0; c < 80; c++) {
      if (pgm_read_byte(&WORLD_MAP[r * 10 + c / 8]) & (0x80 >> (c % 8))) ui->fillRect(c * 3, y0 + r * 3, 3, 3, CLAUDE_LAND);
    }
  }
  if (locationKnown) { // you are here
    ui->fillCircle(mapX(myLon), mapY(myLat, y0), 2, CLAUDE_BLUE);
    ui->drawCircle(mapX(myLon), mapY(myLat, y0), 5, CLAUDE_BLUE);
  }
}
const char *weatherText(int c) {
  if (c == 0) return "Clear sky";
  if (c <= 2) return "Partly cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Foggy";
  if (c <= 57) return "Drizzle";
  if (c <= 67) return "Rain";
  if (c <= 77) return "Snow";
  if (c <= 82) return "Showers";
  if (c <= 86) return "Snow showers";
  return "Thunderstorm";
}
// Sun / moon / cloud / rain / snow / storm, built from circles (s = scale)
void drawWeatherIcon(int cx, int cy, float s, int code, bool day) {
  bool fog = code == 45 || code == 48;
  bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
  bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
  bool storm = code >= 95;
  if (code <= 2) {
    int ox = code == 0 ? 0 : (int)(-8 * s), oy = code == 0 ? 0 : (int)(-6 * s);
    if (day) {
      ui->fillCircle(cx + ox, cy + oy, 11 * s, CLAUDE_AMBER);
      for (int i = 0; i < 8; i++) {
        float a = i * 45 * DEG_TO_RAD;
        ui->drawLine(cx + ox + cosf(a) * 15 * s, cy + oy + sinf(a) * 15 * s, cx + ox + cosf(a) * 21 * s, cy + oy + sinf(a) * 21 * s, CLAUDE_AMBER);
      }
    } else {
      ui->fillCircle(cx + ox, cy + oy, 11 * s, CLAUDE_CREAM);
      ui->fillCircle(cx + ox + 6 * s, cy + oy - 4 * s, 9 * s, CLAUDE_DARK);
    }
  }
  if (fog) {
    for (int i = -1; i <= 1; i++) ui->fillRoundRect(cx - 18 * s, cy + i * 7 * s, 36 * s, 4 * s + 1, 2, CLAUDE_CLOUD);
    return;
  }
  if (code >= 1) {
    uint16_t c = code >= 3 ? CLAUDE_CLOUD : CLAUDE_CREAM;
    ui->fillCircle(cx - 8 * s, cy + 4 * s, 8 * s, c);
    ui->fillCircle(cx + 2 * s, cy - 1 * s, 11 * s, c);
    ui->fillCircle(cx + 12 * s, cy + 5 * s, 7 * s, c);
    ui->fillRoundRect(cx - 16 * s, cy + 4 * s, 35 * s, 9 * s, 4, c);
  }
  for (int i = 0; i < 3; i++) {
    int x = cx + (int)((i - 1) * 9 * s);
    if (rain) {
      ui->drawLine(x, cy + 17 * s, x - 3 * s, cy + 25 * s, CLAUDE_BLUE);
      ui->drawLine(x + 1, cy + 17 * s, x + 1 - 3 * s, cy + 25 * s, CLAUDE_BLUE);
    }
    if (snow) ui->fillCircle(x, cy + 21 * s, 2 * s + 1, CLAUDE_CREAM);
  }
  if (storm) {
    ui->fillTriangle(cx + 2 * s, cy + 12 * s, cx - 6 * s, cy + 24 * s, cx + 1 * s, cy + 22 * s, CLAUDE_AMBER);
    ui->fillTriangle(cx + 1 * s, cy + 20 * s, cx + 6 * s, cy + 20 * s, cx - 3 * s, cy + 32 * s, CLAUDE_AMBER);
  }
}
void drawCentered(const String &text, int y, int size, uint16_t color) {
  ui->setTextSize(size);
  ui->setTextColor(color);
  ui->setCursor(120 - text.length() * 3 * size, y);
  ui->print(text);
}
void drawWeather() {
  drawCentered(myCity.length() ? myCity : String("Your area"), 26, 1, CLAUDE_GRAY);
  drawWeatherIcon(62, 72, 1.5f, wx.code, wx.isDay);
  String t = String((int)roundf(wx.temp));
  ui->setTextSize(5);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(118, 48);
  ui->print(t);
  int ex = 118 + t.length() * 30;
  ui->drawCircle(ex + 5, 52, 4, CLAUDE_CREAM);
  ui->drawCircle(ex + 5, 52, 3, CLAUDE_CREAM);
  ui->setTextSize(2);
  ui->setCursor(ex + 12, 50);
  ui->print("C");
  drawCentered(weatherText(wx.code), 108, 2, CLAUDE_ORANGE);
  char line[48];
  snprintf(line, sizeof(line), "Feels %dC  Humidity %d%%  Wind %dkm/h", (int)roundf(wx.feels), wx.humidity, (int)roundf(wx.wind));
  drawCentered(line, 130, 1, CLAUDE_GRAY);
  struct tm now;
  bool clock = localNow(&now);
  const char *DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for (int i = 0; i < 3; i++) {
    int x = 8 + i * 76;
    ui->drawRoundRect(x, 146, 72, 76, 8, CLAUDE_TRACK);
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_CREAM);
    String day = clock ? String(DAYS[(now.tm_wday + i + 1) % 7]) : "Day " + String(i + 1);
    ui->setCursor(x + 36 - day.length() * 3, 152);
    ui->print(day);
    drawWeatherIcon(x + 36, 180, 0.6f, wx.dCode[i], true);
    snprintf(line, sizeof(line), "%d/%d", (int)roundf(wx.dMax[i]), (int)roundf(wx.dMin[i]));
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(x + 36 - strlen(line) * 3, 206);
    ui->print(line);
  }
}
void drawPlanes() {
  const int cx = 120, cy = 92, R = 60;
  for (int r = 1; r <= 3; r++) ui->drawCircle(cx, cy, R * r / 3, CLAUDE_TRACK);
  ui->drawFastHLine(cx - R, cy, 2 * R, CLAUDE_TRACK);
  ui->drawFastVLine(cx, cy - R, 2 * R, CLAUDE_TRACK);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(cx - 2, cy - R - 9);
  ui->print("N");
  ui->setCursor(cx + R - 6, cy + R - 4);
  ui->printf("%dkm", (int)PLANE_RANGE_KM);
  // radar sweep with a fading tail
  float sweep = (millis() / 8 % 360) * DEG_TO_RAD;
  for (int k = 5; k >= 0; k--) {
    float a = sweep - k * 0.07f;
    ui->drawLine(cx, cy, cx + sinf(a) * R, cy - cosf(a) * R, k == 0 ? CLAUDE_ORANGE : CLAUDE_TRACK);
  }
  for (int i = planeCount - 1; i >= 0; i--) {
    Plane &p = planes[i];
    if (p.dist > PLANE_RANGE_KM) continue;
    float r = p.dist / PLANE_RANGE_KM * R, b = p.bearing * DEG_TO_RAD, h = p.track * DEG_TO_RAD;
    int px = cx + sinf(b) * r, py = cy - cosf(b) * r;
    ui->drawLine(px, py, px + sinf(h) * 7, py - cosf(h) * 7, CLAUDE_CREAM); // heading
    ui->fillCircle(px, py, i == 0 ? 3 : 2, CLAUDE_ORANGE);
    if (i == 0) ui->drawCircle(px, py, 6, CLAUDE_ORANGE);
  }
  ui->fillCircle(cx, cy, 3, CLAUDE_BLUE);
  if (planeCount == 0) {
    drawCentered("No planes in the sky nearby", 172, 1, CLAUDE_GRAY);
    return;
  }
  for (int i = 0; i < 3 && i < planeCount; i++) {
    Plane &p = planes[i];
    int y = 160 + i * 20;
    ui->setTextSize(1);
    ui->setTextColor(i == 0 ? CLAUDE_ORANGE : CLAUDE_CREAM);
    ui->setCursor(8, y);
    ui->printf("%-8s", p.call);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(66, y);
    ui->printf("%3dkm  %sft  %dkm/h", (int)p.dist, withCommas(p.altFt, false).c_str(), p.speed);
  }
}
void drawIss() {
  const int y0 = 34;
  drawWorldMap(y0);
  for (int i = 0; i < issTrailLen; i++) ui->fillRect(mapX(issTrailLon[i]), mapY(issTrailLat[i], y0), 2, 2, CLAUDE_CREAM);
  int x = mapX(issLon), y = mapY(issLat, y0);
  ui->drawCircle(x, y, 6 + (millis() / 120) % 8, CLAUDE_ORANGE);
  ui->fillRect(x - 9, y - 1, 6, 3, CLAUDE_BLUE); // solar panels
  ui->fillRect(x + 4, y - 1, 6, 3, CLAUDE_BLUE);
  ui->fillRect(x - 2, y - 2, 5, 5, CLAUDE_ORANGE);
  char line[48];
  snprintf(line, sizeof(line), "%.1f%c  %.1f%c", fabsf(issLat), issLat >= 0 ? 'N' : 'S', fabsf(issLon), issLon >= 0 ? 'E' : 'W');
  drawCentered(line, 160, 2, CLAUDE_CREAM);
  snprintf(line, sizeof(line), "Altitude %d km   Speed %s km/h", (int)issAlt, withCommas((long)issSpeed, false).c_str());
  drawCentered(line, 182, 1, CLAUDE_GRAY);
  if (locationKnown) {
    float d = distanceKm(myLat, myLon, issLat, issLon);
    snprintf(line, sizeof(line), "%s km from you", withCommas((long)d, false).c_str());
    drawCentered(d < 1500 ? String("Overhead-ish! Look up!") : String(line), 196, 1, d < 1500 ? CLAUDE_ORANGE : CLAUDE_GRAY);
  }
  drawCentered(issDaylight ? "In sunlight" : "In Earth's shadow", 210, 1, CLAUDE_GRAY);
}
String agoText(int64_t secs) {
  if (secs < 3600) return String((int)(secs / 60)) + "m";
  if (secs < 86400) return String((int)(secs / 3600)) + "h";
  return String((int)(secs / 86400)) + "d";
}
void drawQuakes() {
  const int y0 = 34;
  drawWorldMap(y0);
  for (int i = quakeCount - 1; i >= 0; i--) { // oldest first, newest on top
    Quake &q = quakes[i];
    uint16_t c = q.mag >= 6 ? CLAUDE_RED : q.mag >= 5 ? CLAUDE_AMBER : CLAUDE_ORANGE;
    int r = max(2, (int)((q.mag - 4) * 2.5f));
    ui->fillCircle(mapX(q.lon), mapY(q.lat, y0), r, c);
    if (i == 0) ui->drawCircle(mapX(q.lon), mapY(q.lat, y0), r + 2 + (millis() / 150) % 6, c);
  }
  if (quakeCount == 0) {
    drawCentered("No M4.5+ quakes in the last day", 172, 1, CLAUDE_GRAY);
    return;
  }
  time_t now = timeKnown ? time(nullptr) : 0;
  for (int i = 0; i < 3 && i < quakeCount; i++) {
    Quake &q = quakes[i];
    int y = 160 + i * 19;
    uint16_t c = q.mag >= 6 ? CLAUDE_RED : q.mag >= 5 ? CLAUDE_AMBER : CLAUDE_ORANGE;
    ui->fillRoundRect(6, y, 32, 15, 4, c);
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_DARK);
    ui->setCursor(10, y + 4);
    ui->printf("M%.1f", q.mag);
    String place = q.place;
    if (place.length() > 28) place = place.substring(0, 27) + ".";
    ui->setTextColor(CLAUDE_CREAM);
    ui->setCursor(44, y + 4);
    ui->print(place);
    if (now) {
      String ago = agoText(now - q.timeMs / 1000);
      ui->setTextColor(CLAUDE_GRAY);
      ui->setCursor(234 - ago.length() * 6, y + 4);
      ui->print(ago);
    }
  }
  char line[40];
  snprintf(line, sizeof(line), "%d quakes M4.5+ in the last 24h", quakeCount);
  drawCentered(line, 217, 1, CLAUDE_GRAY);
}
void drawCrypto() {
  for (int c = 0; c < 4; c++) {
    int y = 28 + c * 48;
    ui->drawRoundRect(4, y, 232, 44, 8, CLAUDE_TRACK);
    ui->fillCircle(24, y + 22, 13, COIN_COLORS[c]);
    ui->setTextSize(2);
    ui->setTextColor(CLAUDE_DARK);
    ui->setCursor(19, y + 15);
    ui->print(COIN_SYMS[c][0]);
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(44, y + 6);
    ui->printf("%s  %s", COIN_NAMES[c], COIN_SYMS[c]);
    String price = coinPrice[c] >= 1000 ? withCommas((long)coinPrice[c], true) : String(coinPrice[c], 2);
    ui->setTextSize(2);
    ui->setTextColor(CLAUDE_CREAM);
    ui->setCursor(44, y + 20);
    ui->print("Rs" + price);
    // sparkline of the prices seen since you opened this page
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < coinHistLen; i++) {
      lo = min(lo, coinHist[c][i]);
      hi = max(hi, coinHist[c][i]);
    }
    uint16_t trend = coinChange[c] >= 0 ? CLAUDE_GREEN : CLAUDE_RED;
    for (int i = 1; i < coinHistLen; i++) {
      float range = hi > lo ? hi - lo : 1;
      int x0 = 186 + (i - 1) * 44 / (COIN_HIST - 1), x1 = 186 + i * 44 / (COIN_HIST - 1);
      int y0 = y + 24 - (int)((coinHist[c][i - 1] - lo) / range * 18);
      int y1 = y + 24 - (int)((coinHist[c][i] - lo) / range * 18);
      ui->drawLine(x0, y0, x1, y1, trend);
    }
    char chg[12];
    snprintf(chg, sizeof(chg), "%+.2f%%", coinChange[c]);
    ui->setTextSize(1);
    ui->setTextColor(trend);
    ui->setCursor(232 - strlen(chg) * 6, y + 31);
    ui->print(chg);
  }
}
// ---- the newer pages: air quality, sun & moon, space weather, launches, and the Daily Feed ----
// Text from the web can hold accents and curly quotes; the screen font is plain ASCII
String asciiFold(const String &in) {
  static const char *LATIN1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy"; // U+00C0..U+00FF
  String out;
  out.reserve(in.length());
  for (unsigned i = 0; i < in.length(); i++) {
    uint8_t c = in[i];
    if (c < 0x80) {
      out += (char)c;
    } else if (c == 0xC3 && i + 1 < in.length()) { // Latin-1 letters: drop the accent
      uint8_t d = in[++i];
      if (d >= 0x80 && d <= 0xBF) out += LATIN1[d - 0x80];
    } else if (c == 0xE2 && i + 2 < in.length() && (uint8_t)in[i + 1] == 0x80) { // punctuation
      uint8_t d = in[i + 2];
      i += 2;
      if (d == 0x98 || d == 0x99) out += '\'';
      else if (d == 0x9C || d == 0x9D) out += '"';
      else if (d == 0x93 || d == 0x94) out += '-';
      else if (d == 0xA6) out += "...";
      else if (d == 0xA2) out += '*';
    } else if (c >= 0xC0) { // any other character: skip all its bytes
      int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      i += extra;
    }
  }
  return out;
}
// Open Trivia DB sends %-encoded text (no HTML entities to untangle)
String urlDecode(const char *s) {
  String out;
  for (; *s; s++) {
    if (*s == '%' && isxdigit(s[1]) && isxdigit(s[2])) {
      char hex[3] = {s[1], s[2], 0};
      out += (char)strtol(hex, NULL, 16);
      s += 2;
    } else {
      out += *s;
    }
  }
  return out;
}
// "2026-10-06T08:17:44Z" -> seconds since 1970 (UTC), without needing the TZ setting
int64_t isoToEpoch(const char *s) {
  int Y, M, D, h = 0, m = 0, sec = 0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 3) return 0;
  Y -= M <= 2;
  int64_t era = (Y >= 0 ? Y : Y - 399) / 400;
  int yoe = Y - era * 400;
  int doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (era * 146097 + doe - 719468) * 86400LL + h * 3600 + m * 60 + sec;
}
// Draws word-wrapped text; returns the y below it
int drawWrapped(const String &text, int x, int y, int size, uint16_t color, int maxLines, int skip) {
  String lines[30];
  int perLine = (236 - x) / (6 * size);
  int n = wrapText(text, perLine, lines, min(30, skip + maxLines));
  ui->setTextSize(size);
  ui->setTextColor(color);
  for (int i = skip; i < n; i++) {
    ui->setCursor(x, y);
    ui->print(lines[i]);
    y += size * 8 + 3;
  }
  return y;
}
void drawTile(int x, int y, int w, int h, const char *label, const String &value, uint16_t color) {
  ui->drawRoundRect(x, y, w, h, 7, CLAUDE_TRACK);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(x + 8, y + 5);
  ui->print(label);
  ui->setTextSize(2);
  ui->setTextColor(color);
  ui->setCursor(x + 8, y + 17);
  ui->print(value);
}

// Air quality
struct { int aqi; float pm25, pm10, o3, no2; } air;
// Sun & Moon
String sunRise, sunSet;
int sunRiseMin, sunSetMin, dayLenMin;
float uvMax;
// Space weather: the planetary K index, every 3 hours
#define KP_KEEP 24
float kpHist[KP_KEEP];
int kpCount = 0;
// Rocket launches
struct Launch { String rocket, mission, provider, place, status; int64_t net; };
#define MAX_LAUNCHES 3
Launch launches[MAX_LAUNCHES];
int launchCount = 0;
// YouTube
String ytTitle;
long long ytSubs = -1, ytViews = 0, ytVideos = 0, ytFirstSubs = -1;
bool ytHidden = false;
unsigned long ytPartyUntil = 0; // confetti when the count goes up
// Currency (rates against the US dollar)
const char *FX_CODES[] = {"USD", "EUR", "GBP", "JPY", "AUD", "CNY", HOME_CURRENCY};
#define FX_COUNT 6 // the ones shown; the last entry is your home currency
float fxRate[FX_COUNT + 1];
String fxDate;
// Hacker News
#define MAX_NEWS 6
String newsTitle[MAX_NEWS];
int newsPoints[MAX_NEWS], newsComments[MAX_NEWS], newsCount = 0;
// On this day
#define MAX_HISTORY 8
String histYear[MAX_HISTORY], histText[MAX_HISTORY];
int histCount = 0, histSel = 0;
// Wikipedia
String wikiTitle, wikiDesc, wikiText;
int wikiScroll = 0;
// Trivia
String triviaQ, triviaA[4], triviaCat;
int triviaRight = 0, triviaSel = 0, triviaChosen = -1, triviaScore = 0, triviaAsked = 0;
// Joke
String jokeSetup, jokePunch;
bool jokeShown = false;
unsigned long jokeShownAt = 0;

bool youtubeConfigured() {
  return strlen(YT_API_KEY) > 0 && strncmp(YT_API_KEY, "your-", 5) != 0 && strncmp(YT_HANDLE, "@your", 5) != 0;
}
// False when a page can't be fetched yet (and says why on screen)
bool pageReady(int w) {
  if (w == W_YOUTUBE) return youtubeConfigured();
  if (w == W_HISTORY) return timeKnown; // needs today's date
  return true;
}
String feedUrl(int w) {
  char url[320];
  switch (w) {
    case W_AIR:
      snprintf(url, sizeof(url),
               "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.3f&longitude=%.3f"
               "&current=us_aqi,pm2_5,pm10,ozone,nitrogen_dioxide&timezone=auto",
               myLat, myLon);
      return url;
    case W_SKY:
      snprintf(url, sizeof(url),
               "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
               "&daily=sunrise,sunset,daylight_duration,uv_index_max&timezone=auto&forecast_days=1",
               myLat, myLon);
      return url;
    case W_SPACEWX:
      return "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json";
    case W_LAUNCH: // 15 requests an hour without a key, so it refreshes every 30 minutes
      return "https://ll.thespacedevs.com/2.2.0/launch/upcoming/?limit=3&mode=list";
    case W_YOUTUBE: {
      String handle = YT_HANDLE;
      handle.replace("@", "%40");
      return "https://www.googleapis.com/youtube/v3/channels?part=statistics,snippet&forHandle=" + handle +
             "&fields=items(snippet(title),statistics)&key=" + String(YT_API_KEY);
    }
    case W_FX: {
      String symbols;
      for (int i = 1; i <= FX_COUNT; i++) {
        if (symbols.length()) symbols += ",";
        symbols += FX_CODES[i];
      }
      return "https://api.frankfurter.dev/v1/latest?base=USD&symbols=" + symbols;
    }
    case W_NEWS:
      return "https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=6"
             "&attributesToRetrieve=title,points,num_comments&attributesToHighlight=";
    case W_HISTORY: {
      struct tm t;
      if (!localNow(&t)) return "";
      snprintf(url, sizeof(url), "https://byabbe.se/on-this-day/%d/%d/events.json", t.tm_mon + 1, t.tm_mday);
      return url;
    }
    case W_WIKI:
      return "https://en.wikipedia.org/api/rest_v1/page/random/summary"; // redirects to a random article
    case W_TRIVIA:
      return "https://opentdb.com/api.php?amount=1&type=multiple&encode=url3986";
    default: // W_JOKE
      return "https://v2.jokeapi.dev/joke/Programming,Misc,Pun?safe-mode&type=twopart";
  }
}
// Which fields to keep from each reply (some feeds are big)
void feedFilter(int w, JsonDocument &f) {
  switch (w) {
    case W_SPACEWX:
      f[0]["Kp"] = true;
      break;
    case W_LAUNCH:
      for (const char *k : {"name", "net", "location", "lsp_name"}) f["results"][0][k] = true;
      f["results"][0]["status"]["abbrev"] = true;
      break;
    case W_NEWS:
      for (const char *k : {"title", "points", "num_comments"}) f["hits"][0][k] = true;
      break;
    case W_HISTORY:
      f["events"][0]["year"] = true;
      f["events"][0]["description"] = true;
      break;
    case W_WIKI:
      for (const char *k : {"title", "description", "extract"}) f[k] = true;
      break;
    default:
      break; // small replies: keep everything
  }
}
bool parseFeed(int w, JsonDocument &doc) {
  switch (w) {
    case W_AIR: {
      JsonObject c = doc["current"];
      air.aqi = c["us_aqi"] | -1;
      air.pm25 = c["pm2_5"] | 0.0f;
      air.pm10 = c["pm10"] | 0.0f;
      air.o3 = c["ozone"] | 0.0f;
      air.no2 = c["nitrogen_dioxide"] | 0.0f;
      if (air.aqi < 0) return wError[w] = "No air data for here", false;
      return true;
    }
    case W_SKY: {
      String rise = doc["daily"]["sunrise"][0] | "", set = doc["daily"]["sunset"][0] | "";
      if (rise.length() < 16 || set.length() < 16) return wError[w] = "No sunrise data", false;
      sunRise = rise.substring(11, 16);
      sunSet = set.substring(11, 16);
      sunRiseMin = sunRise.substring(0, 2).toInt() * 60 + sunRise.substring(3).toInt();
      sunSetMin = sunSet.substring(0, 2).toInt() * 60 + sunSet.substring(3).toInt();
      dayLenMin = (int)((doc["daily"]["daylight_duration"][0] | 0.0f) / 60);
      uvMax = doc["daily"]["uv_index_max"][0] | 0.0f;
      return true;
    }
    case W_SPACEWX: {
      JsonArray rows = doc.as<JsonArray>();
      int n = rows.size(), first = max(0, n - KP_KEEP);
      kpCount = 0;
      for (int i = first; i < n; i++) kpHist[kpCount++] = rows[i]["Kp"] | 0.0f;
      if (!kpCount) return wError[w] = "No space weather data", false;
      return true;
    }
    case W_LAUNCH:
      launchCount = 0;
      for (JsonObject r : doc["results"].as<JsonArray>()) {
        if (launchCount >= MAX_LAUNCHES) break;
        Launch &l = launches[launchCount++];
        String name = asciiFold(r["name"] | "");
        int bar = name.indexOf(" | ");
        l.rocket = bar >= 0 ? name.substring(0, bar) : name;
        l.mission = bar >= 0 ? name.substring(bar + 3) : name;
        l.provider = asciiFold(r["lsp_name"] | "");
        l.place = asciiFold(r["location"] | "");
        l.status = (const char *)(r["status"]["abbrev"] | "");
        l.net = isoToEpoch(r["net"] | "");
      }
      return true;
    case W_YOUTUBE: {
      JsonObject ch = doc["items"][0];
      if (ch.isNull()) return wError[w] = "No channel " + String(YT_HANDLE) + " - check YT_HANDLE", false;
      ytTitle = asciiFold(ch["snippet"]["title"] | "");
      ytHidden = ch["statistics"]["hiddenSubscriberCount"] | false;
      long long subs = atoll(ch["statistics"]["subscriberCount"] | "0");
      ytViews = atoll(ch["statistics"]["viewCount"] | "0");
      ytVideos = atoll(ch["statistics"]["videoCount"] | "0");
      if (ytSubs >= 0 && subs > ytSubs) { // more subscribers: party!
        ytPartyUntil = millis() + 6000;
        if (!danceOn) {
          const int notes[4] = {1046, 1318, 1568, 2093};
          for (int i = 0; i < 4; i++) pycamera.speaker_tone(notes[i], 90);
        }
      }
      ytSubs = subs;
      if (ytFirstSubs < 0) ytFirstSubs = subs;
      return true;
    }
    case W_FX: {
      fxRate[0] = 1.0f; // everything is against USD
      for (int i = 1; i <= FX_COUNT; i++) fxRate[i] = doc["rates"][FX_CODES[i]] | 0.0f;
      fxDate = (const char *)(doc["date"] | "");
      if (strcmp(HOME_CURRENCY, "USD") != 0 && fxRate[FX_COUNT] <= 0) return wError[w] = "No rate for " HOME_CURRENCY, false;
      if (strcmp(HOME_CURRENCY, "USD") == 0) fxRate[FX_COUNT] = 1.0f;
      return true;
    }
    case W_NEWS:
      newsCount = 0;
      for (JsonObject h : doc["hits"].as<JsonArray>()) {
        if (newsCount >= MAX_NEWS) break;
        newsTitle[newsCount] = asciiFold(h["title"] | "");
        newsPoints[newsCount] = h["points"] | 0;
        newsComments[newsCount++] = h["num_comments"] | 0;
      }
      return true;
    case W_HISTORY: {
      JsonArray ev = doc["events"];
      int n = ev.size();
      if (!n) return wError[w] = "Nothing for today", false;
      // a random handful, kept in order through history
      int pick[MAX_HISTORY], k = 0;
      for (int i = 0; i < n && k < MAX_HISTORY; i++)
        if ((int)(esp_random() % (n - i)) < MAX_HISTORY - k) pick[k++] = i;
      histCount = k;
      for (int i = 0; i < k; i++) {
        histYear[i] = (const char *)(ev[pick[i]]["year"] | "");
        histText[i] = asciiFold(ev[pick[i]]["description"] | "");
      }
      histSel = 0;
      return true;
    }
    case W_WIKI:
      wikiTitle = asciiFold(doc["title"] | "");
      wikiDesc = asciiFold(doc["description"] | "");
      wikiText = asciiFold(doc["extract"] | "");
      wikiScroll = 0;
      return true;
    case W_TRIVIA: {
      JsonObject q = doc["results"][0];
      if (q.isNull()) return wError[w] = "No question came back", false;
      triviaQ = asciiFold(urlDecode(q["question"] | ""));
      triviaCat = asciiFold(urlDecode(q["category"] | "")) + " - " + urlDecode(q["difficulty"] | "");
      triviaRight = esp_random() % 4; // shuffle the right answer in among the others
      for (int i = 0, wrong = 0; i < 4; i++)
        triviaA[i] = asciiFold(urlDecode(i == triviaRight ? (q["correct_answer"] | "") : (q["incorrect_answers"][wrong++] | "")));
      triviaSel = 0;
      triviaChosen = -1;
      return true;
    }
    case W_JOKE:
      if (doc["error"] | true) return wError[w] = "No joke today, sorry", false;
      jokeSetup = asciiFold(doc["setup"] | "");
      jokePunch = asciiFold(doc["delivery"] | "");
      jokeShown = false;
      return true;
  }
  return true;
}

// ---- drawing the newer pages
void aqiLevel(int aqi, const char **name, uint16_t *color) {
  if (aqi <= 50) *name = "Good", *color = CLAUDE_GREEN;
  else if (aqi <= 100) *name = "Moderate", *color = CLAUDE_AMBER;
  else if (aqi <= 150) *name = "Unhealthy for some", *color = CLAUDE_ORANGE;
  else if (aqi <= 200) *name = "Unhealthy", *color = CLAUDE_RED;
  else if (aqi <= 300) *name = "Very unhealthy", *color = 0x881F;
  else *name = "Hazardous", *color = 0x8000;
}
void drawAir() {
  const char *name;
  uint16_t color;
  aqiLevel(air.aqi, &name, &color);
  drawCentered(myCity.length() ? myCity : String("Your area"), 28, 1, CLAUDE_GRAY);
  drawCentered(String(air.aqi), 42, 5, color);
  drawCentered("US AQI", 84, 1, CLAUDE_GRAY);
  drawCentered(name, 96, 2, color);
  // scale from 0 to 300+, coloured by band, with a marker at today's value
  const int bands[6] = {50, 100, 150, 200, 300, 400};
  const uint16_t cols[6] = {CLAUDE_GREEN, CLAUDE_AMBER, CLAUDE_ORANGE, CLAUDE_RED, 0x881F, 0x8000};
  for (int i = 0, x0 = 12; i < 6; i++) {
    int x1 = 12 + 216 * bands[i] / 400;
    ui->fillRect(x0, 120, x1 - x0 - 1, 6, cols[i]);
    x0 = x1;
  }
  int mx = 12 + 216 * min(air.aqi, 399) / 400;
  ui->fillTriangle(mx, 128, mx - 5, 135, mx + 5, 135, CLAUDE_CREAM);
  char v[12];
  snprintf(v, sizeof(v), "%.0f", air.pm25);
  drawTile(8, 142, 108, 36, "PM2.5 ug/m3", v, CLAUDE_CREAM);
  snprintf(v, sizeof(v), "%.0f", air.pm10);
  drawTile(124, 142, 108, 36, "PM10 ug/m3", v, CLAUDE_CREAM);
  snprintf(v, sizeof(v), "%.0f", air.o3);
  drawTile(8, 184, 108, 36, "Ozone", v, CLAUDE_CREAM);
  snprintf(v, sizeof(v), "%.0f", air.no2);
  drawTile(124, 184, 108, 36, "NO2", v, CLAUDE_CREAM);
}
// The Moon's age in days since the last new moon (one cycle is 29.53 days)
float moonAge(time_t now) {
  const double synodic = 29.530588853;
  double age = fmod((now - 947182440.0) / 86400.0, synodic); // new moon of 6 Jan 2000, 18:14 UTC
  return age < 0 ? age + synodic : age;
}
void drawMoon(int cx, int cy, int r, float age) {
  float phase = age / 29.530588853f * 2 * PI; // 0 new, PI full
  ui->fillCircle(cx, cy, r, CLAUDE_TRACK);
  for (int y = -r; y <= r; y++) {
    float w = sqrtf(r * r - y * y), k = cosf(phase);
    float x0 = phase < PI ? w * k : -w, x1 = phase < PI ? w : -w * k; // lit part, waxing or waning
    if (x1 > x0) ui->drawFastHLine(cx + (int)x0, cy + y, (int)(x1 - x0) + 1, CLAUDE_CREAM);
  }
}
void drawSky() {
  struct tm t;
  bool clock = localNow(&t);
  // the day as an arc from sunrise (left) to sunset (right)
  const int cx = 120, cy = 112, R = 84;
  for (int a = 0; a <= 180; a += 6) {
    float r = a * DEG_TO_RAD;
    ui->fillCircle(cx - cosf(r) * R, cy - sinf(r) * R * 0.75f, 1, CLAUDE_TRACK);
  }
  int nowMin = clock ? t.tm_hour * 60 + t.tm_min : -1;
  if (nowMin >= sunRiseMin && nowMin <= sunSetMin && sunSetMin > sunRiseMin) {
    float r = (float)(nowMin - sunRiseMin) / (sunSetMin - sunRiseMin) * PI;
    int sx = cx - cosf(r) * R, sy = cy - sinf(r) * R * 0.75f;
    ui->fillCircle(sx, sy, 9, CLAUDE_AMBER);
    ui->drawCircle(sx, sy, 12 + (millis() / 300) % 3, CLAUDE_AMBER);
  } else {
    drawCentered("Night", 70, 2, CLAUDE_GRAY);
  }
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_AMBER);
  ui->setCursor(16, 118);
  ui->print("Rise " + sunRise);
  ui->setCursor(224 - 8 * 6, 118);
  ui->print("Set " + sunSet);
  char line[40];
  snprintf(line, sizeof(line), "%dh %02dm of daylight", dayLenMin / 60, dayLenMin % 60);
  drawCentered(line, 92, 1, CLAUDE_CREAM);
  snprintf(line, sizeof(line), "UV up to %.0f%s", uvMax, uvMax >= 8 ? " - very high!" : uvMax >= 6 ? " - high" : "");
  drawCentered(line, 134, 1, uvMax >= 6 ? CLAUDE_ORANGE : CLAUDE_GRAY);
  ui->drawFastHLine(12, 148, 216, CLAUDE_TRACK);
  // tonight's moon, worked out on the board
  if (!timeKnown) {
    drawCentered("Moon: waiting for the clock", 180, 1, CLAUDE_GRAY);
    return;
  }
  float age = moonAge(time(nullptr));
  drawMoon(52, 186, 26, age);
  const char *NAMES[] = {"New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
                         "Full moon", "Waning gibbous", "Last quarter", "Waning crescent"};
  int idx = (int)(age / 29.530588853f * 8 + 0.5f) % 8;
  int lit = (int)roundf((1 - cosf(age / 29.530588853f * 2 * PI)) / 2 * 100);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(92, 168);
  ui->print(NAMES[idx]);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(92, 182);
  ui->printf("%d%% lit", lit);
  float toFull = fmodf(14.765f - age + 29.53f, 29.53f);
  ui->setCursor(92, 196);
  if (toFull < 0.5f || toFull > 29.0f) ui->print("Full moon tonight!");
  else ui->printf("Full moon in %d days", (int)roundf(toFull));
}
uint16_t kpColor(float kp) {
  return kp >= 7 ? CLAUDE_RED : kp >= 5 ? CLAUDE_ORANGE : kp >= 4 ? CLAUDE_AMBER : CLAUDE_GREEN;
}
void drawSpaceWeather() {
  float kp = kpHist[kpCount - 1];
  char big[8];
  snprintf(big, sizeof(big), "%.1f", kp);
  drawCentered("Planetary K index", 28, 1, CLAUDE_GRAY);
  drawCentered(big, 40, 5, kpColor(kp));
  int g = kp >= 9 ? 5 : kp >= 8 ? 4 : kp >= 7 ? 3 : kp >= 6 ? 2 : kp >= 5 ? 1 : 0;
  String level = g ? "G" + String(g) + " geomagnetic storm" : String(kp >= 4 ? "Active" : kp >= 3 ? "Unsettled" : "Quiet");
  drawCentered(level, 84, 2, kpColor(kp));
  // roughly how far from the poles the northern/southern lights reach at this Kp
  float auroraLat = 66.5f - 2.4f * kp;
  char line[44];
  snprintf(line, sizeof(line), "Aurora possible above ~%.0f deg latitude", auroraLat);
  drawCentered(line, 108, 1, CLAUDE_CREAM);
  if (locationKnown) {
    bool visible = fabsf(myLat) >= auroraLat;
    drawCentered(visible ? "You might see it tonight - look up!" : "Too close to the equator here", 120, 1,
                 visible ? CLAUDE_GREEN : CLAUDE_GRAY);
  }
  // the last three days, one bar every three hours
  const int base = 206, h = 60;
  for (int i = 0; i < kpCount; i++) {
    int bh = max(2, (int)(kpHist[i] / 9.0f * h));
    ui->fillRect(12 + i * 9, base - bh, 7, bh, kpColor(kpHist[i]));
  }
  ui->drawFastHLine(10, base - (int)(5 / 9.0f * h), 220, CLAUDE_TRACK); // storm line at Kp 5
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(12, 211);
  ui->print("3 days ago");
  ui->setCursor(204, 211);
  ui->print("now");
}
String countdownText(int64_t secs) {
  char buf[24];
  bool past = secs < 0;
  int64_t s = past ? -secs : secs;
  int d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60, sec = s % 60;
  if (d > 0) snprintf(buf, sizeof(buf), "T%c %dd %02d:%02d:%02d", past ? '+' : '-', d, h, m, sec);
  else snprintf(buf, sizeof(buf), "T%c %02d:%02d:%02d", past ? '+' : '-', h, m, sec);
  return buf;
}
void drawLaunches() {
  if (!launchCount) {
    drawCentered("No launches scheduled", 110, 1, CLAUDE_GRAY);
    return;
  }
  Launch &l = launches[0];
  int y = drawWrapped(l.mission, 8, 30, 2, CLAUDE_CREAM, 2, 0);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_ORANGE);
  ui->setCursor(8, y + 2);
  ui->print(l.rocket.substring(0, 37));
  y = drawWrapped(l.provider + " - " + l.place, 8, y + 14, 1, CLAUDE_GRAY, 2, 0);
  if (timeKnown && l.net) drawCentered(countdownText(l.net - (int64_t)time(nullptr)), y + 8, 2, CLAUDE_AMBER);
  uint16_t sc = l.status == "Go" ? CLAUDE_GREEN : l.status == "Hold" ? CLAUDE_RED : CLAUDE_GRAY;
  int w = l.status.length() * 6 + 12;
  ui->fillRoundRect(120 - w / 2, y + 30, w, 13, 6, sc);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_DARK);
  ui->setCursor(126 - w / 2, y + 33);
  ui->print(l.status);
  for (int i = 1; i < launchCount; i++) { // the next ones, one line each
    int ly = 182 + (i - 1) * 20;
    ui->drawFastHLine(8, ly - 5, 224, CLAUDE_TRACK);
    ui->setTextColor(CLAUDE_CREAM);
    ui->setCursor(8, ly);
    ui->print(launches[i].mission.substring(0, 24));
    if (timeKnown && launches[i].net) {
      int64_t s = launches[i].net - (int64_t)time(nullptr);
      String when = s < 86400 ? "today" : "in " + String((int)(s / 86400)) + "d";
      ui->setTextColor(CLAUDE_GRAY);
      ui->setCursor(232 - when.length() * 6, ly);
      ui->print(when);
    }
  }
}
// A big number with commas, at the largest size that fits
void drawBigCount(long long v, int y, uint16_t color) {
  String s = withCommas((long)min(v, 2147483647LL), false);
  int size = s.length() <= 6 ? 5 : s.length() <= 9 ? 4 : 3;
  drawCentered(s, y, size, color);
}
void drawYouTube() {
  // the play-button logo
  ui->fillRoundRect(96, 30, 48, 34, 10, CLAUDE_RED);
  ui->fillTriangle(113, 38, 113, 56, 129, 47, 0xFFFF);
  drawCentered(ytTitle.length() > 38 ? ytTitle.substring(0, 37) : ytTitle, 72, 1, CLAUDE_CREAM);
  if (ytHidden) drawCentered("Subscriber count is hidden", 104, 1, CLAUDE_GRAY);
  else drawBigCount(ytSubs, 88, millis() < ytPartyUntil ? CLAUDE_AMBER : 0xFFFF);
  drawCentered("subscribers", 130, 1, CLAUDE_GRAY);
  if (ytFirstSubs >= 0 && ytSubs > ytFirstSubs) {
    drawCentered("+" + withCommas((long)(ytSubs - ytFirstSubs), false) + " since I switched on", 144, 1, CLAUDE_GREEN);
  }
  drawTile(8, 160, 108, 40, "VIEWS", withCommas((long)min(ytViews, 2147483647LL), false), CLAUDE_CREAM);
  drawTile(124, 160, 108, 40, "VIDEOS", String((long)ytVideos), CLAUDE_CREAM);
  drawCentered("YouTube rounds big counts", 210, 1, CLAUDE_TRACK);
  if (millis() < ytPartyUntil) { // confetti and a dancing Clawd
    uint32_t seed = millis() / 120; // new confetti every few frames, without touching random()
    const uint16_t c[4] = {CLAUDE_ORANGE, CLAUDE_AMBER, CLAUDE_GREEN, CLAUDE_BLUE};
    for (int i = 0; i < 30; i++) {
      seed = seed * 1664525u + 1013904223u;
      ui->fillRect((seed >> 8) % 240, 26 + (seed >> 20) % 134, 3, 3, c[i % 4]);
    }
    drawBot(ui, 184, 196, 4, FACE_HAPPY, (millis() / 150) % 2 ? 2 : 1, (millis() / 150) % 2 ? 1 : 2);
  }
}
void drawCurrency() {
  float home = fxRate[FX_COUNT];
  int row = 0;
  for (int i = 0; i < FX_COUNT; i++) {
    if (strcmp(FX_CODES[i], HOME_CURRENCY) == 0 || fxRate[i] <= 0) continue;
    int y = 30 + row * 31;
    if (row++ >= 6) break;
    float value = home / fxRate[i]; // one unit of this currency, in your currency
    ui->drawRoundRect(6, y, 228, 27, 7, CLAUDE_TRACK);
    ui->setTextSize(2);
    ui->setTextColor(CLAUDE_ORANGE);
    ui->setCursor(14, y + 6);
    ui->printf("1 %s", FX_CODES[i]);
    char v[16];
    snprintf(v, sizeof(v), value >= 100 ? "%.1f" : value >= 1 ? "%.2f" : "%.4f", value);
    String text = String(v) + " " + HOME_CURRENCY;
    ui->setTextColor(CLAUDE_CREAM);
    ui->setCursor(226 - text.length() * 12, y + 6);
    ui->print(text);
  }
  drawCentered("European Central Bank rates, " + fxDate, 218, 1, CLAUDE_GRAY);
}
void drawNews() {
  int y = 28;
  for (int i = 0; i < newsCount && y < 214; i++) {
    ui->fillRect(4, y + 1, 2, 18, CLAUDE_ORANGE);
    y = drawWrapped(newsTitle[i], 10, y, 1, CLAUDE_CREAM, 2, 0);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(10, y);
    ui->printf("%d points - %d comments", newsPoints[i], newsComments[i]);
    y += 14;
  }
}
void drawHistory() {
  struct tm t;
  if (localNow(&t)) {
    char date[16];
    strftime(date, sizeof(date), "%d %B", &t);
    drawCentered(date, 28, 1, CLAUDE_GRAY);
  }
  drawCentered("In " + histYear[histSel], 42, 3, CLAUDE_ORANGE);
  String &text = histText[histSel];
  int size = text.length() <= 120 ? 2 : 1;
  drawWrapped(text, 10, 76, size, CLAUDE_CREAM, size == 2 ? 7 : 12, 0);
  char nav[24];
  snprintf(nav, sizeof(nav), "^v %d/%d   OK more", histSel + 1, histCount);
  drawCentered(nav, 216, 1, CLAUDE_GRAY);
}
void drawWiki() {
  int y = drawWrapped(wikiTitle, 8, 28, 2, CLAUDE_CREAM, 2, 0);
  if (wikiDesc.length()) y = drawWrapped(wikiDesc, 8, y + 1, 1, CLAUDE_ORANGE, 2, 0);
  ui->drawFastHLine(8, y + 2, 224, CLAUDE_TRACK);
  int lines = (210 - (y + 8)) / 11;
  drawWrapped(wikiText, 8, y + 8, 1, CLAUDE_GRAY, lines, wikiScroll);
  drawCentered("^v scroll   OK another", 216, 1, CLAUDE_TRACK);
}
void drawTrivia() {
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(8, 28);
  ui->print(triviaCat.substring(0, 28));
  char score[12];
  snprintf(score, sizeof(score), "%d/%d", triviaScore, triviaAsked);
  ui->setTextColor(CLAUDE_ORANGE);
  ui->setCursor(232 - strlen(score) * 6, 28);
  ui->print(score);
  int size = triviaQ.length() <= 64 ? 2 : 1;
  drawWrapped(triviaQ, 8, 42, size, CLAUDE_CREAM, size == 2 ? 4 : 6, 0);
  for (int i = 0; i < 4; i++) {
    int y = 112 + i * 27;
    uint16_t fill = triviaChosen < 0 ? (i == triviaSel ? CLAUDE_ORANGE : 0)
                    : i == triviaRight ? CLAUDE_GREEN
                    : i == triviaChosen ? CLAUDE_RED
                                        : 0;
    if (fill) ui->fillRoundRect(6, y, 228, 23, 8, fill);
    else ui->drawRoundRect(6, y, 228, 23, 8, CLAUDE_TRACK);
    ui->setTextSize(1);
    ui->setTextColor(fill ? CLAUDE_DARK : CLAUDE_CREAM);
    ui->setCursor(14, y + 8);
    ui->print((char)('A' + i));
    ui->print("  ");
    ui->print(triviaA[i].substring(0, 34));
  }
  if (triviaChosen >= 0) drawCentered(triviaChosen == triviaRight ? "Correct! OK: next" : "Not quite. OK: next", 222, 1, CLAUDE_CREAM);
}
void drawJoke() {
  drawBubble(ui, 8, 28, 224, 92, 46, 132);
  int size = jokeSetup.length() <= 90 ? 2 : 1;
  drawWrapped(jokeSetup, 16, 36, size, CLAUDE_DARK, size == 2 ? 4 : 7, 0);
  unsigned long t = millis() - jokeShownAt;
  bool laughing = jokeShown && t < 2500;
  int bob = laughing ? (int)(4 * sinf(t / 60.0f)) : 0;
  drawBot(ui, 22, 140 + bob, 6, jokeShown ? FACE_HAPPY : FACE_LOOK_RIGHT, laughing ? ((t / 150) % 2 ? 2 : 1) : 0, 0);
  if (jokeShown) {
    drawWrapped(jokePunch, 76, 140, jokePunch.length() <= 52 ? 2 : 1, CLAUDE_ORANGE, 6, 0);
  } else {
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(82, 160);
    ui->print("OK: tell me!");
  }
}
// Buttons that pages use for themselves (LEFT/RIGHT always flip pages)
void worldButtons(int w) {
  bool ok = pycamera.justPressed(AWEXP_BUTTON_OK);
  bool up = pycamera.justPressed(AWEXP_BUTTON_UP), down = pycamera.justPressed(AWEXP_BUTTON_DOWN);
  switch (w) {
    case W_TRIVIA:
      if (!wHasData[w]) break;
      if (triviaChosen < 0) {
        if (up) triviaSel = (triviaSel + 3) % 4;
        if (down) triviaSel = (triviaSel + 1) % 4;
        if (ok) {
          triviaChosen = triviaSel;
          triviaAsked++;
          if (triviaChosen == triviaRight) {
            triviaScore++;
            petBoost(3);
            pycamera.speaker_tone(1318, 70);
            pycamera.speaker_tone(1760, 110);
          } else {
            pycamera.speaker_tone(330, 200);
          }
          prefs.putInt("trv_ok", triviaScore);
          prefs.putInt("trv_n", triviaAsked);
        }
        return;
      }
      break; // answered: OK fetches the next question
    case W_JOKE:
      if (ok && wHasData[w] && !jokeShown) {
        jokeShown = true;
        jokeShownAt = millis();
        petBoost(3);
        for (int i = 0; i < 3; i++) pycamera.speaker_tone(1568 + i * 200, 50); // ba-dum-tss
        return;
      }
      break;
    case W_HISTORY:
      if (histCount && up) histSel = (histSel + histCount - 1) % histCount;
      if (histCount && down) histSel = (histSel + 1) % histCount;
      break;
    case W_WIKI:
      if (down) wikiScroll++;
      if (up && wikiScroll > 0) wikiScroll--;
      break;
  }
  if (ok) wFetchedAt[w] = 0; // refresh now
}
// Why a page has nothing to show yet
String worldWaitText(int w) {
  if (wError[w].length()) return wError[w];
  if (w == W_YOUTUBE && !youtubeConfigured()) return "Add YT_HANDLE and YT_API_KEY in YOUR SETTINGS";
  if (w == W_HISTORY && !timeKnown) return "Waiting for the clock...";
  if (needsLocation(w) && !locationKnown)
    return locateTried && webWidget != W_LOCATE ? "Couldn't find your location. Retrying..." : "Asking your phone where we are...";
  if (WiFi.status() != WL_CONNECTED) return "Waiting for WiFi...";
  return "Loading...";
}

void worldMode() {
  const uint8_t *set = feedMode ? FEED_SET : WORLD_SET;
  int n = feedMode ? sizeof(FEED_SET) : sizeof(WORLD_SET);
  int &idx = feedMode ? feedIdx : worldIdx;
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) idx = (idx + n - 1) % n;
  if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) idx = (idx + 1) % n;
  worldPage = set[idx];
  worldButtons(worldPage);
  worldTick();
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 50) return;
  lastDraw = millis();
  int w = worldPage;
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 12, 12, 8, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(26, 5);
  ui->print(W_TITLES[w]);
  // live / loading / error dot
  bool loading = webWidget == w || (webWidget == W_LOCATE && needsLocation(w));
  ui->fillCircle(230, 11, 4, loading ? CLAUDE_AMBER : wError[w].length() ? CLAUDE_RED : wHasData[w] ? CLAUDE_GREEN : CLAUDE_TRACK);
  if (!wHasData[w]) {
    String lines[3];
    int count = wrapText(worldWaitText(w), 36, lines, 3);
    for (int i = 0; i < count; i++) drawCentered(lines[i], 104 + i * 12, 1, CLAUDE_GRAY);
  } else {
    switch (w) {
      case W_WEATHER: drawWeather(); break;
      case W_PLANES: drawPlanes(); break;
      case W_ISS: drawIss(); break;
      case W_QUAKES: drawQuakes(); break;
      case W_CRYPTO: drawCrypto(); break;
      case W_AIR: drawAir(); break;
      case W_SKY: drawSky(); break;
      case W_SPACEWX: drawSpaceWeather(); break;
      case W_LAUNCH: drawLaunches(); break;
      case W_YOUTUBE: drawYouTube(); break;
      case W_FX: drawCurrency(); break;
      case W_NEWS: drawNews(); break;
      case W_HISTORY: drawHistory(); break;
      case W_WIKI: drawWiki(); break;
      case W_TRIVIA: drawTrivia(); break;
      case W_JOKE: drawJoke(); break;
    }
  }
  for (int i = 0; i < n; i++) ui->fillCircle(120 - (n - 1) * 6 + i * 12, 233, i == idx ? 3 : 2, i == idx ? CLAUDE_ORANGE : CLAUDE_TRACK);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(4, 229);
  ui->print("<");
  ui->setCursor(230, 229);
  ui->print(">");
  pushUi();
}

// ===================== GAMES =====================
#define GAME_FLAPPY 0
#define GAME_BLOCKS 1
#define GAME_2048 2
#define GAME_TILT 3
#define GAME_BEATS 4
#define GAME_DUEL 5
#define GAME_COUNT 6
const char *GAME_NAMES[] = {"Flappy Clawd", "Block Drop", "2048", "Tilt Catch", "Clawd Beats", "Pong Duel"};
const char *GAME_HINTS[] = {"OK / Shutter / UP: flap", "</> move  UP turn  DN fast  Shutter drop",
                            "D-pad: slide the tiles", "Tilt the board to steer",
                            "Hit < ^ > on the beat", "2 boards, same WiFi: UP/DN paddle"};
int gameSelection = 0, currentGame = 0;
uint32_t bestScores[GAME_COUNT];
uint32_t gameScore = 0;
bool gameOver = false, newBest = false, gameOverShown = false;
unsigned long gameTick = 0;
void loadBestScores() {
  for (int i = 0; i < GAME_COUNT; i++) bestScores[i] = prefs.getUInt(("best" + String(i)).c_str(), 0);
}
void endGame() {
  gameOver = true;
  petBoost(8);
  newBest = gameScore > bestScores[currentGame];
  if (newBest) {
    bestScores[currentGame] = gameScore;
    prefs.putUInt(("best" + String(currentGame)).c_str(), gameScore);
  }
  pycamera.speaker_tone(newBest ? 1568 : 196, newBest ? 120 : 200);
}
bool isHeld(uint8_t pin) {
  return !(pycamera.button_state & (1UL << pin));
}
void drawGameOver() {
  ui->fillRoundRect(30, 70, 180, 100, 12, CLAUDE_DARK);
  ui->drawRoundRect(30, 70, 180, 100, 12, CLAUDE_ORANGE);
  drawCentered(newBest ? "New best!" : "Game over", 82, 2, CLAUDE_ORANGE);
  drawCentered("Score " + String(gameScore), 110, 2, CLAUDE_CREAM);
  drawCentered("Best " + String(bestScores[currentGame]), 134, 1, CLAUDE_GRAY);
  drawCentered("OK: play again", 152, 1, CLAUDE_GRAY);
}

// ---- Flappy Clawd: flap through the gaps ----
struct Pipe {
  float x;
  int gapY;
  bool scored;
};
Pipe pipes[3];
float birdY, birdVY;
int flapFrames = 0;
const int BIRD_X = 50, PIPE_W = 30, PIPE_GAP = 74;
void flappyStart() {
  birdY = 100;
  birdVY = 0;
  for (int i = 0; i < 3; i++) pipes[i] = {260.0f + i * 120, (int)random(60, 160), false};
}
void flappyUpdate() {
  if (pycamera.justPressed(AWEXP_BUTTON_OK) || pycamera.justPressed(SHUTTER_BUTTON) || pycamera.justPressed(AWEXP_BUTTON_UP)) {
    birdVY = -4.6f;
    flapFrames = 6;
  }
  if (flapFrames) flapFrames--;
  birdVY = min(birdVY + 0.32f, 7.0f);
  birdY += birdVY;
  float speed = min(2.2f + gameScore * 0.05f, 4.0f);
  for (int i = 0; i < 3; i++) {
    Pipe &p = pipes[i];
    p.x -= speed;
    if (p.x < -PIPE_W) {
      p.x += 360;
      p.gapY = random(60, 160);
      p.scored = false;
    }
    if (!p.scored && p.x + PIPE_W < BIRD_X) {
      p.scored = true;
      gameScore++;
      pycamera.speaker_tone(1760, 10);
    }
    // the bot is 28x20 pixels at (BIRD_X, birdY)
    bool overlapX = BIRD_X + 26 > p.x && BIRD_X + 2 < p.x + PIPE_W;
    bool inGap = birdY > p.gapY - PIPE_GAP / 2 && birdY + 20 < p.gapY + PIPE_GAP / 2;
    if (overlapX && !inGap) endGame();
  }
  if (birdY < 0 || birdY + 20 > 214) endGame();
}
void flappyDraw() {
  ui->fillScreen(CLAUDE_DARK);
  for (int i = 0; i < 24; i++) { // drifting stars
    int sx = (i * 97 + 240 - (int)(millis() / 60 * (1 + i % 3)) % 240) % 240;
    ui->drawPixel(sx, (i * 53) % 200 + 4, i % 3 ? CLAUDE_GRAY : CLAUDE_CREAM);
  }
  for (int i = 0; i < 3; i++) {
    Pipe &p = pipes[i];
    int top = p.gapY - PIPE_GAP / 2, bot = p.gapY + PIPE_GAP / 2;
    ui->fillRect(p.x, 0, PIPE_W, top, CLAUDE_ORANGE);
    ui->fillRect(p.x - 3, top - 10, PIPE_W + 6, 10, CLAUDE_CREAM);
    ui->fillRect(p.x, bot, PIPE_W, 214 - bot, CLAUDE_ORANGE);
    ui->fillRect(p.x - 3, bot, PIPE_W + 6, 10, CLAUDE_CREAM);
  }
  ui->fillRect(0, 214, 240, 26, CLAUDE_TRACK);
  for (int x = -(int)(millis() / 15 % 20); x < 240; x += 20) ui->drawFastVLine(x, 214, 26, 0x18C3);
  int arm = flapFrames ? 2 : 1;
  drawBot(ui, BIRD_X, (int)birdY + 4, 4, gameOver ? FACE_DEAD : birdVY > 3 ? FACE_WORRIED : FACE_HAPPY, arm, arm);
  drawCentered(String(gameScore), 10, 3, CLAUDE_CREAM);
}

// ---- Block Drop: falling blocks, clear full lines ----
#define BW 10
#define BH 20
#define CELL 11
// Each piece in its 4 rotations, as a 4x4 bitmask (top-left = 0x8000)
const uint16_t PIECES[7][4] = {
  {0x0F00, 0x2222, 0x00F0, 0x4444}, {0x44C0, 0x8E00, 0x6440, 0x0E20}, {0x4460, 0x0E80, 0xC440, 0x2E00},
  {0xCC00, 0xCC00, 0xCC00, 0xCC00}, {0x06C0, 0x8C40, 0x6C00, 0x4620}, {0x0E40, 0x4C40, 0x4E00, 0x4640},
  {0x0C60, 0x4C80, 0xC600, 0x2640}};
const uint16_t PIECE_COLORS[7] = {CLAUDE_ORANGE, CLAUDE_BLUE, CLAUDE_AMBER, CLAUDE_CREAM, CLAUDE_GREEN, 0xA37A, CLAUDE_RED};
uint8_t board[BH][BW]; // 0 = empty, else piece + 1
int pieceType, pieceRot, pieceX, pieceY, nextPiece, linesDone;
unsigned long lastFall = 0, happyUntil = 0;
bool pieceFits(int type, int rot, int px, int py) {
  uint16_t m = PIECES[type][rot];
  for (int i = 0; i < 16; i++) {
    if (!(m & (0x8000 >> i))) continue;
    int x = px + i % 4, y = py + i / 4;
    if (x < 0 || x >= BW || y >= BH) return false;
    if (y >= 0 && board[y][x]) return false;
  }
  return true;
}
void blocksSpawn() {
  pieceType = nextPiece;
  nextPiece = random(7);
  pieceRot = 0;
  pieceX = 3;
  pieceY = -1;
  if (!pieceFits(pieceType, pieceRot, pieceX, pieceY)) endGame();
}
void blocksStart() {
  memset(board, 0, sizeof(board));
  linesDone = 0;
  nextPiece = random(7);
  blocksSpawn();
}
void blocksLock() {
  uint16_t m = PIECES[pieceType][pieceRot];
  for (int i = 0; i < 16; i++) {
    if (!(m & (0x8000 >> i))) continue;
    int x = pieceX + i % 4, y = pieceY + i / 4;
    if (y < 0) { // locked above the top
      endGame();
      return;
    }
    board[y][x] = pieceType + 1;
  }
  int cleared = 0;
  for (int y = BH - 1; y >= 0; y--) {
    bool full = true;
    for (int x = 0; x < BW; x++) full = full && board[y][x];
    if (!full) continue;
    memmove(board[1], board[0], y * BW);
    memset(board[0], 0, BW);
    cleared++;
    y++; // check the row that moved down into this one
  }
  if (cleared) {
    static const int POINTS[5] = {0, 40, 100, 300, 1200};
    gameScore += POINTS[cleared] * (linesDone / 10 + 1);
    linesDone += cleared;
    happyUntil = millis() + 800;
    pycamera.speaker_tone(1046 + cleared * 200, 40);
  }
  blocksSpawn();
}
void blocksUpdate() {
  if (pressedOrRepeat(AWEXP_BUTTON_LEFT) && pieceFits(pieceType, pieceRot, pieceX - 1, pieceY)) pieceX--;
  if (pressedOrRepeat(AWEXP_BUTTON_RIGHT) && pieceFits(pieceType, pieceRot, pieceX + 1, pieceY)) pieceX++;
  if (pycamera.justPressed(AWEXP_BUTTON_UP) || pycamera.justPressed(AWEXP_BUTTON_OK)) {
    int rot = (pieceRot + 1) % 4;
    static const int KICKS[5] = {0, -1, 1, -2, 2}; // nudge sideways if turning hits a wall
    for (int k = 0; k < 5; k++) {
      if (pieceFits(pieceType, rot, pieceX + KICKS[k], pieceY)) {
        pieceRot = rot;
        pieceX += KICKS[k];
        break;
      }
    }
  }
  if (pycamera.justPressed(SHUTTER_BUTTON)) { // hard drop
    while (pieceFits(pieceType, pieceRot, pieceX, pieceY + 1)) {
      pieceY++;
      gameScore += 2;
    }
    blocksLock();
    return;
  }
  unsigned long interval = isHeld(AWEXP_BUTTON_DOWN) ? 40 : max(90, 650 - (linesDone / 10) * 55);
  if (millis() - lastFall > interval) {
    lastFall = millis();
    if (pieceFits(pieceType, pieceRot, pieceX, pieceY + 1)) pieceY++;
    else blocksLock();
  }
}
void drawCell(int x, int y, int size, uint16_t c) {
  ui->fillRect(x, y, size - 1, size - 1, c);
  ui->drawFastHLine(x, y, size - 1, CLAUDE_CREAM); // little highlight
}
void blocksDraw() {
  ui->fillScreen(CLAUDE_DARK);
  const int ox = 8, oy = 9;
  ui->drawRect(ox - 2, oy - 2, BW * CELL + 3, BH * CELL + 3, CLAUDE_TRACK);
  for (int y = 0; y < BH; y++)
    for (int x = 0; x < BW; x++)
      if (board[y][x]) drawCell(ox + x * CELL, oy + y * CELL, CELL, PIECE_COLORS[board[y][x] - 1]);
  // ghost shows where the piece will land
  int gy = pieceY;
  while (pieceFits(pieceType, pieceRot, pieceX, gy + 1)) gy++;
  uint16_t m = PIECES[pieceType][pieceRot];
  for (int i = 0; i < 16; i++) {
    if (!(m & (0x8000 >> i))) continue;
    int x = pieceX + i % 4;
    if (gy + i / 4 >= 0) ui->drawRect(ox + x * CELL, oy + (gy + i / 4) * CELL, CELL - 1, CELL - 1, CLAUDE_GRAY);
    if (pieceY + i / 4 >= 0) drawCell(ox + x * CELL, oy + (pieceY + i / 4) * CELL, CELL, PIECE_COLORS[pieceType]);
  }
  // side panel
  const int px = 132;
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(px, 10);
  ui->print("NEXT");
  uint16_t nm = PIECES[nextPiece][0];
  for (int i = 0; i < 16; i++)
    if (nm & (0x8000 >> i)) drawCell(px + (i % 4) * 9, 24 + (i / 4) * 9, 9, PIECE_COLORS[nextPiece]);
  const char *LABELS[3] = {"SCORE", "LINES", "LEVEL"};
  uint32_t vals[3] = {gameScore, (uint32_t)linesDone, (uint32_t)(linesDone / 10 + 1)};
  for (int i = 0; i < 3; i++) {
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(px, 70 + i * 36);
    ui->print(LABELS[i]);
    ui->setTextSize(2);
    ui->setTextColor(CLAUDE_CREAM);
    ui->setCursor(px, 82 + i * 36);
    ui->print(vals[i]);
  }
  drawBot(ui, px + 22, 196, 7, gameOver ? FACE_DEAD : millis() < happyUntil ? FACE_HAPPY : FACE_NORMAL,
          millis() < happyUntil ? 2 : 0, millis() < happyUntil ? 2 : 0);
}

// ---- 2048: slide and merge tiles ----
uint8_t tiles[4][4]; // 0 = empty, n = tile 2^n
bool reached2048 = false;
unsigned long winBannerUntil = 0;
int newTileX = -1, newTileY = -1;
unsigned long newTileAt = 0;
void spawnTile() {
  int empty[16], n = 0;
  for (int i = 0; i < 16; i++)
    if (!tiles[i / 4][i % 4]) empty[n++] = i;
  if (!n) return;
  int i = empty[random(n)];
  tiles[i / 4][i % 4] = random(10) ? 1 : 2;
  newTileX = i % 4;
  newTileY = i / 4;
  newTileAt = millis();
}
void tilesStart() {
  memset(tiles, 0, sizeof(tiles));
  reached2048 = false;
  spawnTile();
  spawnTile();
}
// Slides one row/column toward c[0], merging equal neighbours once
bool slideLine(uint8_t *c[4]) {
  uint8_t vals[4], out[4] = {0, 0, 0, 0};
  int n = 0, o = 0;
  for (int i = 0; i < 4; i++)
    if (*c[i]) vals[n++] = *c[i];
  for (int i = 0; i < n; i++) {
    if (i + 1 < n && vals[i] == vals[i + 1]) {
      out[o++] = vals[i] + 1;
      gameScore += 1UL << (vals[i] + 1);
      if (vals[i] + 1 == 11 && !reached2048) {
        reached2048 = true;
        winBannerUntil = millis() + 2500;
      }
      i++;
    } else {
      out[o++] = vals[i];
    }
  }
  bool moved = false;
  for (int i = 0; i < 4; i++) {
    if (*c[i] != out[i]) moved = true;
    *c[i] = out[i];
  }
  return moved;
}
bool slideTiles(int dx, int dy) {
  bool moved = false;
  for (int k = 0; k < 4; k++) {
    uint8_t *c[4];
    for (int i = 0; i < 4; i++) {
      int x = dx < 0 ? i : dx > 0 ? 3 - i : k;
      int y = dy < 0 ? i : dy > 0 ? 3 - i : k;
      c[i] = &tiles[y][x];
    }
    moved |= slideLine(c);
  }
  return moved;
}
bool tilesCanMove() {
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) {
      if (!tiles[y][x]) return true;
      if (x < 3 && tiles[y][x] == tiles[y][x + 1]) return true;
      if (y < 3 && tiles[y][x] == tiles[y + 1][x]) return true;
    }
  return false;
}
void tilesUpdate() {
  int dx = 0, dy = 0;
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) dx = -1;
  else if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) dx = 1;
  else if (pycamera.justPressed(AWEXP_BUTTON_UP)) dy = -1;
  else if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) dy = 1;
  if (!dx && !dy) return;
  if (slideTiles(dx, dy)) {
    spawnTile();
    pycamera.speaker_tone(880, 8);
    if (!tilesCanMove()) endGame();
  }
}
void tilesDraw() {
  static const uint16_t COLORS[12] = {0x2103, CLAUDE_CREAM, 0xF6B8, 0xED0F, CLAUDE_ORANGE, 0xCB07,
                                      0xB245, CLAUDE_AMBER, 0xEDC7, 0xE629, CLAUDE_GREEN, CLAUDE_BLUE};
  ui->fillScreen(CLAUDE_DARK);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_ORANGE);
  ui->setCursor(14, 6);
  ui->print("2048");
  String s = String(gameScore);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(226 - s.length() * 12, 6);
  ui->print(s);
  const int ox = 14, oy = 26, T = 48, G = 4;
  ui->fillRoundRect(ox, oy, 4 * T + 5 * G, 4 * T + 5 * G, 8, 0x18C3);
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) {
      uint8_t v = tiles[y][x];
      int tx = ox + G + x * (T + G), ty = oy + G + y * (T + G), size = T;
      if (x == newTileX && y == newTileY && millis() - newTileAt < 150) { // new tiles pop in
        int shrink = (150 - (millis() - newTileAt)) / 10;
        tx += shrink;
        ty += shrink;
        size -= 2 * shrink;
      }
      ui->fillRoundRect(tx, ty, size, size, 6, COLORS[min((int)v, 11)]);
      if (!v) continue;
      String label = String(1UL << v);
      int ts = label.length() <= 2 ? 3 : label.length() <= 3 ? 2 : 1;
      ui->setTextSize(ts);
      ui->setTextColor(v <= 3 ? CLAUDE_DARK : CLAUDE_CREAM);
      ui->setCursor(tx + size / 2 - label.length() * 3 * ts, ty + size / 2 - 4 * ts);
      ui->print(label);
    }
  if (millis() < winBannerUntil) {
    ui->fillRoundRect(40, 100, 160, 40, 10, CLAUDE_ORANGE);
    drawCentered("2048! Keep going", 114, 1, CLAUDE_DARK);
  }
}

// ---- Tilt Catch: tilt to collect sparks and dodge bugs ----
// Tilt is measured against how you held the board when the game started. If the
// ball moves the wrong way for you, flip these.
#define TILT_FLIP_X false
#define TILT_FLIP_Y false
float ballX, ballY, ballVX, ballVY;
float tiltBase[3];
int tiltDownAxis = 2;
float sparkX, sparkY;
struct Bug {
  float x, y, vx, vy;
};
#define MAX_BUGS 6
Bug bugs[MAX_BUGS];
int bugCount = 0, lives = 3;
unsigned long hurtUntil = 0;
void addBug() {
  if (bugCount >= MAX_BUGS) return;
  float a = random(360) * DEG_TO_RAD, sp = 1.0f + bugCount * 0.25f;
  bugs[bugCount++] = {(float)random(20, 220), (float)(random(2) ? 40 : 200), cosf(a) * sp, sinf(a) * sp};
}
void placeSpark() {
  sparkX = random(20, 220);
  sparkY = random(40, 220);
}
void tiltStart() {
  float a[3] = {0, 0, 0}, g[3];
  for (int i = 0; i < 8; i++) { // average a few readings as "level"
    if (readAccelG(g)) {
      a[0] += g[0];
      a[1] += g[1];
      a[2] += g[2];
    }
    delay(5);
  }
  tiltDownAxis = 0;
  for (int i = 0; i < 3; i++) {
    tiltBase[i] = a[i] / 8;
    if (fabsf(tiltBase[i]) > fabsf(tiltBase[tiltDownAxis])) tiltDownAxis = i;
  }
  ballX = 120;
  ballY = 130;
  ballVX = ballVY = 0;
  bugCount = 0;
  lives = 3;
  addBug();
  placeSpark();
}
void tiltUpdate() {
  float a[3];
  if (readAccelG(a)) {
    // gravity sits on one axis; the other two are left-right and up-down tilt
    int hx = tiltDownAxis == 0 ? 1 : 0, vy = 3 - tiltDownAxis - hx;
    float tx = (a[hx] - tiltBase[hx]) * (TILT_FLIP_X ? -1 : 1);
    float ty = (a[vy] - tiltBase[vy]) * (TILT_FLIP_Y ? -1 : 1);
    ballVX = (ballVX + tx * 2.2f) * 0.94f;
    ballVY = (ballVY + ty * 2.2f) * 0.94f;
  }
  ballX += ballVX;
  ballY += ballVY;
  if (ballX < 10 || ballX > 230) {
    ballVX = -ballVX * 0.5f;
    ballX = constrain(ballX, 10, 230);
  }
  if (ballY < 34 || ballY > 230) {
    ballVY = -ballVY * 0.5f;
    ballY = constrain(ballY, 34, 230);
  }
  if (sq(ballX - sparkX) + sq(ballY - sparkY) < sq(18)) {
    gameScore++;
    pycamera.speaker_tone(1568, 15);
    placeSpark();
    if (gameScore % 4 == 0) addBug();
  }
  for (int i = 0; i < bugCount; i++) {
    Bug &b = bugs[i];
    b.x += b.vx;
    b.y += b.vy;
    if (b.x < 8 || b.x > 232) b.vx = -b.vx;
    if (b.y < 34 || b.y > 232) b.vy = -b.vy;
    if (millis() > hurtUntil && sq(ballX - b.x) + sq(ballY - b.y) < sq(16)) {
      lives--;
      hurtUntil = millis() + 1500;
      pycamera.speaker_tone(150, 120);
      if (lives <= 0) endGame();
    }
  }
}
void tiltDraw() {
  ui->fillScreen(CLAUDE_DARK);
  for (int x = 0; x < 240; x += 24) ui->drawFastVLine(x, 30, 210, 0x18C3);
  for (int y = 30; y < 240; y += 24) ui->drawFastHLine(0, y, 240, 0x18C3);
  drawClaudeLogo(ui, sparkX, sparkY, 9, millis() / 4, CLAUDE_ORANGE);
  for (int i = 0; i < bugCount; i++) { // little red beetles
    Bug &b = bugs[i];
    ui->fillCircle(b.x, b.y, 7, CLAUDE_RED);
    ui->drawFastVLine(b.x, b.y - 7, 14, CLAUDE_DARK);
    ui->fillCircle(b.x, b.y - 7, 3, CLAUDE_DARK);
    for (int l = -1; l <= 1; l++) {
      ui->drawLine(b.x - 7, b.y + l * 4, b.x - 11, b.y + l * 5, CLAUDE_RED);
      ui->drawLine(b.x + 7, b.y + l * 4, b.x + 11, b.y + l * 5, CLAUDE_RED);
    }
  }
  if (millis() > hurtUntil || (millis() / 100) % 2) { // blink while hurt
    ui->fillCircle(ballX, ballY, 9, CLAUDE_CREAM);
    ui->fillCircle(ballX - 3, ballY - 3, 3, CLAUDE_GRAY);
  }
  ui->fillRect(0, 0, 240, 28, CLAUDE_DARK);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(8, 6);
  ui->print(gameScore);
  for (int i = 0; i < 3; i++) ui->fillCircle(226 - i * 16, 13, 5, i < lives ? CLAUDE_ORANGE : CLAUDE_TRACK);
}

void startGame(int g) {
  currentGame = g;
  gameScore = 0;
  gameOver = false;
  gameOverShown = false;
  randomSeed(esp_random());
  switch (g) {
    case GAME_FLAPPY: flappyStart(); break;
    case GAME_BLOCKS: blocksStart(); break;
    case GAME_2048: tilesStart(); break;
    case GAME_TILT: tiltStart(); break;
    case GAME_BEATS: beatsStart(); break;
    case GAME_DUEL: duelStart(); break;
  }
}
void drawGameMenu() {
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print("Games");
  for (int i = 0; i < GAME_COUNT; i++) {
    int y = 34 + i * 28;
    bool sel = i == gameSelection;
    if (sel) ui->fillRoundRect(10, y, 220, 24, 8, CLAUDE_ORANGE);
    else ui->drawRoundRect(10, y, 220, 24, 8, CLAUDE_TRACK);
    ui->setTextSize(2);
    ui->setTextColor(sel ? CLAUDE_DARK : CLAUDE_CREAM);
    ui->setCursor(20, y + 5);
    ui->print(GAME_NAMES[i]);
    String best = "Best " + String(bestScores[i]);
    ui->setTextSize(1);
    ui->setTextColor(sel ? CLAUDE_DARK : CLAUDE_GRAY);
    ui->setCursor(222 - best.length() * 6, y + 9);
    ui->print(best);
  }
  drawCentered(GAME_HINTS[gameSelection], 206, 1, CLAUDE_GRAY);
  drawCentered("OK play   SEL menu", 224, 1, CLAUDE_GRAY);
  pushUi();
}
void gameMenuMode() {
  if (pycamera.justPressed(AWEXP_BUTTON_UP)) gameSelection = (gameSelection + GAME_COUNT - 1) % GAME_COUNT;
  if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) gameSelection = (gameSelection + 1) % GAME_COUNT;
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
    startGame(gameSelection);
    currentMode = MODE_GAME;
    return;
  }
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 100) return;
  lastDraw = millis();
  drawGameMenu();
}
void drawGame() {
  switch (currentGame) {
    case GAME_FLAPPY: flappyDraw(); break;
    case GAME_BLOCKS: blocksDraw(); break;
    case GAME_2048: tilesDraw(); break;
    case GAME_TILT: tiltDraw(); break;
    case GAME_BEATS: beatsDraw(); break;
    case GAME_DUEL: duelDraw(); break;
  }
  if (gameOver) drawGameOver();
  pushUi();
}
void gameMode() {
  if (gameOver) {
    if (pycamera.justPressed(AWEXP_BUTTON_OK) || pycamera.justPressed(SHUTTER_BUTTON)) startGame(currentGame);
    else if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) currentMode = MODE_GAME_MENU;
    else if (!gameOverShown) { // draw the final frame with the score card once
      drawGame();
      gameOverShown = true;
    }
    return;
  }
  // 2048 and Block Drop react to every press; the action games tick at a steady 30 fps
  if (currentGame == GAME_2048) tilesUpdate();
  if (currentGame == GAME_BLOCKS) blocksUpdate();
  if (currentGame == GAME_BEATS) beatsUpdate(); // judged on every press, not just every frame
  if (gameOver || millis() - gameTick < 33) return;
  gameTick = millis();
  if (currentGame == GAME_FLAPPY) flappyUpdate();
  if (currentGame == GAME_TILT) tiltUpdate();
  if (currentGame == GAME_DUEL) duelUpdate();
  if (!gameOver) drawGame();
}

// ===================== CLAWD PET =====================
// A little Tamagotchi. Clawd slowly gets hungry, sleepy and bored (even while the board is
// off, if the clock is known). Feed it, play, tickle it, or put it down for a nap. Using the
// rest of the board (games, dancing, focus sessions, photos) cheers it up too.
struct PetState {
  float food, energy, fun;
  int64_t savedAt;
  bool napping;
};
PetState pet = {80, 80, 80, 0, false};
unsigned long petTickAt = 0, petSavedAt = 0;
#define PET_EAT 1
#define PET_PLAY 2
#define PET_TICKLE 3
#define PET_FULL 4
#define PET_TIRED 5
int petAnim = 0; // one of the above while an animation plays
unsigned long petAnimAt = 0;
const unsigned long PET_ANIM_MS[] = {0, 1400, 2000, 1200, 1200, 1200};
void savePet() {
  pet.savedAt = timeKnown ? (int64_t)time(nullptr) : 0;
  prefs.putBytes("pet", &pet, sizeof(pet));
  petSavedAt = millis();
}
void petDecay(float minutes) {
  pet.food = max(0.0f, pet.food - 0.12f * minutes);
  pet.fun = max(0.0f, pet.fun - 0.15f * minutes);
  pet.energy = pet.napping ? min(100.0f, pet.energy + 0.6f * minutes) : max(0.0f, pet.energy - 0.08f * minutes);
}
void loadPet() {
  if (prefs.getBytesLength("pet") != sizeof(pet)) return;
  prefs.getBytes("pet", &pet, sizeof(pet));
  if (timeKnown && pet.savedAt > 0) { // catch up on the time the board was off (3 days max)
    float mins = (time(nullptr) - pet.savedAt) / 60.0f;
    if (mins > 0) petDecay(min(mins, 3 * 1440.0f));
  }
}
void petBoost(float fun) {
  pet.fun = min(100.0f, pet.fun + fun);
}
// Called every loop(): ages the pet a little and saves it now and then
void petTick() {
  if (petTickAt && millis() - petTickAt < 10000) return;
  if (petTickAt) petDecay((millis() - petTickAt) / 60000.0f);
  petTickAt = millis();
  if (pet.napping && pet.energy >= 100) pet.napping = false; // fully rested
  if (millis() - petSavedAt > 5UL * 60 * 1000) savePet();
}
void startPetAnim(int anim) {
  petAnim = anim;
  petAnimAt = millis();
}
void drawPetBar(int x, float v, int icon) {
  const int y = 8;
  if (icon == 0) { // cookie
    ui->fillCircle(x + 6, y + 4, 6, 0xCC8B);
    ui->fillCircle(x + 4, y + 2, 1, CLAUDE_DARK);
    ui->fillCircle(x + 8, y + 6, 1, CLAUDE_DARK);
  } else if (icon == 1) { // lightning bolt
    ui->fillTriangle(x + 7, y - 2, x + 2, y + 5, x + 7, y + 5, CLAUDE_AMBER);
    ui->fillTriangle(x + 5, y + 3, x + 10, y + 3, x + 4, y + 11, CLAUDE_AMBER);
  } else { // spark = fun
    drawClaudeLogo(ui, x + 6, y + 4, 7, 0, CLAUDE_CREAM);
  }
  uint16_t c = v > 60 ? CLAUDE_GREEN : v > 30 ? CLAUDE_AMBER : CLAUDE_RED;
  ui->fillRoundRect(x + 16, y, 54, 8, 4, CLAUDE_TRACK);
  ui->fillRoundRect(x + 16, y, max(8, (int)(54 * v / 100)), 8, 4, c);
}
void drawPet() {
  unsigned long now = millis(), t = now - petAnimAt;
  if (petAnim && t > PET_ANIM_MS[petAnim]) petAnim = 0;
  ui->fillScreen(pet.napping ? 0x0841 : CLAUDE_DARK);
  // window: day or night outside
  struct tm tm;
  bool day = !localNow(&tm) || (tm.tm_hour >= 6 && tm.tm_hour < 18);
  ui->fillRoundRect(170, 30, 58, 44, 6, day && !pet.napping ? 0x3B74 : 0x1107);
  if (day) ui->fillCircle(212, 44, 7, CLAUDE_AMBER);
  else {
    ui->fillCircle(212, 44, 7, CLAUDE_CREAM);
    ui->fillCircle(216, 41, 6, 0x1107);
  }
  ui->drawRoundRect(170, 30, 58, 44, 6, CLAUDE_CREAM);
  ui->drawFastVLine(199, 30, 44, CLAUDE_CREAM);
  ui->drawFastHLine(170, 52, 58, CLAUDE_CREAM);
  ui->fillRect(0, 200, 240, 40, 0x2104); // floor
  drawPetBar(4, pet.food, 0);
  drawPetBar(82, pet.energy, 1);
  drawPetBar(160, pet.fun, 2);
  // Clawd: face and pose depend on how it's doing and what it's doing
  const int U = BOT_U;
  int x = (240 - 7 * U) / 2, y = 130 + (int)(2 * sinf(now / 400.0f));
  float worst = min(pet.food, min(pet.energy, pet.fun));
  Face face = worst > 60 ? FACE_HAPPY : worst > 30 ? FACE_NORMAL : FACE_WORRIED;
  if (now % 3500 < 140) face = FACE_BLINK;
  int left = 0, right = 0;
  String says = pet.food < 40 && pet.food <= pet.energy && pet.food <= pet.fun ? "I'm hungry! Got a cookie?"
              : pet.energy < 30 && pet.energy <= pet.fun                     ? "So sleepy... nap time?"
              : pet.fun < 40                                                 ? "I'm bored. Play with me!"
                                                                             : "Life is good!";
  switch (pet.napping ? -1 : petAnim) {
    case -1: // napping
      face = FACE_BLINK;
      says = "Zzz...";
      break;
    case PET_EAT: { // cookie drops into Clawd's mouth, then crunch
      face = FACE_HAPPY;
      left = right = 1;
      float p = min(1.0f, t / 600.0f);
      int cy = 40 + (int)(p * 95), r = t < 600 ? 10 : max(0, 10 - (int)((t - 600) / 80));
      if (r > 0) {
        ui->fillCircle(120, cy, r, 0xCC8B);
        ui->fillCircle(116, cy - 3, 2, CLAUDE_DARK);
        ui->fillCircle(124, cy + 2, 2, CLAUDE_DARK);
      }
      if (t > 600) y += ((t / 100) % 2) * 3; // chomp chomp
      says = "Nom nom nom!";
      break;
    }
    case PET_PLAY: { // ball bounces, Clawd jumps after it
      y -= (int)(18 * fabsf(sinf(t / 160.0f)));
      left = right = 2;
      face = FACE_HAPPY;
      int bx = 30 + (int)(t / 2000.0f * 180), by = 190 - (int)(60 * fabsf(sinf(t / 200.0f)));
      ui->fillCircle(bx, by, 8, CLAUDE_RED);
      ui->drawFastHLine(bx - 8, by, 16, CLAUDE_CREAM);
      says = "Wheee!";
      break;
    }
    case PET_TICKLE:
      x += (int)(6 * sinf(t / 40.0f));
      face = FACE_HAPPY;
      left = right = 1;
      says = "Hehe, that tickles!";
      break;
    case PET_FULL:
      face = (t / 150) % 2 ? FACE_LOOK_LEFT : FACE_LOOK_RIGHT;
      says = "I'm full, thanks!";
      break;
    case PET_TIRED:
      face = FACE_WORRIED;
      says = "Too tired to play...";
      break;
  }
  drawBot(ui, x, y, U, face, right, left);
  if (pet.napping) { // nightcap + floating Zs
    ui->fillTriangle(x + U, y, x + 5 * U, y, x + 6 * U, y - 2 * U, CLAUDE_BLUE);
    ui->fillCircle(x + 6 * U, y - 2 * U, U / 2, CLAUDE_CREAM);
    for (int i = 0; i < 3; i++) {
      float p = ((now / 40 + i * 33) % 100) / 100.0f;
      ui->setTextSize(1 + (int)(p * 3));
      ui->setTextColor(CLAUDE_CREAM);
      ui->setCursor(x + 7 * U + (int)(p * 20), y - 10 - (int)(p * 50));
      ui->print('z');
    }
  }
  drawBubble(ui, 8, 30, 150, 44, 70, 118);
  String lines[2];
  int n = wrapText(says, 22, lines, 2);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_DARK);
  for (int i = 0; i < n; i++) {
    ui->setCursor(18, 40 + i * 13 + (n == 1 ? 6 : 0));
    ui->print(lines[i]);
  }
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  if (pet.napping) drawCentered("DOWN or OK: wake Clawd up", 226, 1, CLAUDE_GRAY);
  else drawCentered("OK feed  > play  ^ tickle  v nap", 226, 1, CLAUDE_GRAY);
  pushUi();
}
void petMode() {
  if (pet.napping) {
    if (pycamera.justPressed(AWEXP_BUTTON_DOWN) || pycamera.justPressed(AWEXP_BUTTON_OK)) {
      pet.napping = false;
      savePet();
      pycamera.speaker_tone(1320, 40);
    }
  } else if (!petAnim) {
    if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
      if (pet.food > 95) {
        startPetAnim(PET_FULL);
      } else {
        pet.food = min(100.0f, pet.food + 25);
        startPetAnim(PET_EAT);
        pycamera.speaker_tone(660, 30);
      }
      savePet();
    } else if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) {
      if (pet.energy < 10) {
        startPetAnim(PET_TIRED);
      } else {
        pet.fun = min(100.0f, pet.fun + 20);
        pet.energy = max(0.0f, pet.energy - 5);
        startPetAnim(PET_PLAY);
        pycamera.speaker_tone(1046, 40);
      }
      savePet();
    } else if (pycamera.justPressed(AWEXP_BUTTON_UP)) {
      petBoost(5);
      startPetAnim(PET_TICKLE);
      pycamera.speaker_tone(1568, 25);
      pycamera.speaker_tone(1760, 25);
    } else if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) {
      pet.napping = true;
      savePet();
    }
  }
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 33) return;
  lastDraw = millis();
  drawPet();
}

// ===================== TOOLBOX =====================
#define TOOL_POMODORO 0
#define TOOL_LEVEL 1
#define TOOL_BOOTH 2
#define TOOL_TIMELAPSE 3
#define TOOL_QR 4
#define TOOL_HOME 5
#define TOOL_REMOTE 6
#define TOOL_AI 7
#define TOOL_HUNT 8
#define TOOL_COUNT 9
const char *TOOL_NAMES[] = {"Pomodoro", "Spirit Level", "Photo Booth", "Timelapse", "QR Scanner", "Home Controls",
                            "Phone Remote", "Person Detector", "Scavenger Hunt"};
const char *TOOL_HINTS[] = {"Focus timer with breaks", "Is it straight?", "3-2-1 photo to the SD card",
                            "Record a timelapse, play it back", "Read QR codes (phone companion)",
                            "Your light, fan and robots", "Control me from your phone",
                            "A neural net on the board, offline", "Claude writes riddles, you hide QR codes"};
int toolSelection = 0, currentTool = 0;
// Title + scrolling list of 6 rows, used by the Toolbox menu
void drawListScreen(const char *title, const char *const *items, int count, int sel, const char *hint) {
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print(title);
  const int VISIBLE = 6;
  int first = constrain(sel - 2, 0, max(0, count - VISIBLE));
  for (int i = first; i < first + VISIBLE && i < count; i++) {
    int y = 38 + (i - first) * 28;
    bool selected = i == sel;
    if (selected) ui->fillRoundRect(10, y, 220, 24, 8, CLAUDE_ORANGE);
    else ui->drawRoundRect(10, y, 220, 24, 8, CLAUDE_TRACK);
    ui->setTextColor(selected ? CLAUDE_DARK : CLAUDE_CREAM);
    ui->setCursor(22, y + 5);
    ui->print(items[i]);
  }
  if (first > 0) ui->fillTriangle(220, 14, 214, 22, 226, 22, CLAUDE_GRAY);
  if (first + VISIBLE < count) ui->fillTriangle(220, 214, 214, 206, 226, 206, CLAUDE_GRAY);
  drawCentered(hint, 212, 1, CLAUDE_GRAY);
  drawCentered("OK open   SEL menu", 228, 1, CLAUDE_GRAY);
  pushUi();
}
// A thin bar across the top of a camera screen, with the tool's name
void drawCameraTitle(PyCameraFB *fb, const char *title) {
  fb->fillRect(0, 0, 240, 18, CLAUDE_DARK);
  drawClaudeLogo(fb, 10, 9, 6, 0, CLAUDE_ORANGE);
  fb->setTextSize(1);
  fb->setTextColor(CLAUDE_CREAM);
  fb->setCursor(20, 5);
  fb->print(title);
}
void drawCameraHint(PyCameraFB *fb, const String &hint, uint16_t color) {
  fb->fillRect(0, 222, 240, 18, CLAUDE_DARK);
  fb->setTextSize(1);
  fb->setTextColor(color);
  fb->setCursor(120 - hint.length() * 3, 227);
  fb->print(hint);
}

// ---- Pomodoro: 25 min focus, 5 min break, keeps running on other screens ----
int pomoWorkMin = 25, pomoBreakMin = 5, pomoSessions = 0;
bool pomoRunning = false, pomoOnBreak = false;
unsigned long pomoEndsAt = 0, pomoLeftMs = 25UL * 60000;
void pomoReset() {
  pomoRunning = false;
  pomoOnBreak = false;
  pomoLeftMs = pomoWorkMin * 60000UL;
}
void pomoStart() {
  pomoEndsAt = millis() + pomoLeftMs;
  pomoRunning = true;
}
unsigned long pomoLeft() {
  if (!pomoRunning) return pomoLeftMs;
  return pomoEndsAt > millis() ? pomoEndsAt - millis() : 0;
}
// Called every loop(): switches between focus and break and chimes, on any screen
void pomoTick() {
  if (!pomoRunning || millis() < pomoEndsAt) return;
  if (!pomoOnBreak) {
    pomoSessions++;
    petBoost(10);
  }
  pomoOnBreak = !pomoOnBreak;
  pomoEndsAt = millis() + (pomoOnBreak ? pomoBreakMin : pomoWorkMin) * 60000UL;
  if (currentMode == MODE_SLEEP) wakeUp();
  if (!danceOn) {
    const int up[3] = {784, 988, 1175}, down[3] = {1175, 988, 784};
    for (int i = 0; i < 3; i++) pycamera.speaker_tone(pomoOnBreak ? up[i] : down[i], 120);
  }
}
void drawPomodoro() {
  ui->fillScreen(CLAUDE_DARK);
  unsigned long left = pomoLeft(), total = (pomoOnBreak ? pomoBreakMin : pomoWorkMin) * 60000UL;
  float done = 1.0f - (float)left / total;
  uint16_t col = pomoOnBreak ? CLAUDE_GREEN : CLAUDE_ORANGE;
  const int cx = 120, cy = 104, R = 80;
  for (int a = 0; a < 360; a += 4) { // progress ring
    float r = a * DEG_TO_RAD;
    ui->fillCircle(cx + sinf(r) * R, cy - cosf(r) * R, 4, a < done * 360 ? col : CLAUDE_TRACK);
  }
  drawCentered(pomoOnBreak ? "BREAK" : "FOCUS", 50, 2, col);
  char tbuf[8];
  snprintf(tbuf, sizeof(tbuf), "%02lu:%02lu", left / 60000, left / 1000 % 60);
  drawCentered(tbuf, 72, 4, CLAUDE_CREAM);
  // Clawd types away while you focus, stretches on breaks
  unsigned long now = millis();
  int arm = !pomoRunning ? 0 : pomoOnBreak ? 2 : (now / 150) % 2 ? 1 : 0;
  Face face = !pomoRunning ? FACE_BLINK : pomoOnBreak ? FACE_HAPPY : FACE_NORMAL;
  drawBot(ui, cx - 17, 120, 5, face, arm, pomoOnBreak ? 2 : !pomoRunning ? 0 : 1 - arm);
  if (!pomoRunning) drawCentered(left == total ? "ready" : "paused", 154, 1, CLAUDE_GRAY);
  // one tomato per finished focus session
  int shown = min(pomoSessions, 10);
  for (int i = 0; i < shown; i++) {
    int x = 120 - shown * 9 + 9 + i * 18;
    ui->fillCircle(x, 200, 6, CLAUDE_RED);
    ui->fillRect(x - 1, 192, 3, 3, CLAUDE_GREEN);
  }
  char hint[48];
  snprintf(hint, sizeof(hint), "OK %s   < reset   UP/DN %d min", pomoRunning ? "pause" : "start", pomoWorkMin);
  drawCentered(hint, 226, 1, CLAUDE_GRAY);
  pushUi();
}
void pomodoroTool() {
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
    if (pomoRunning) {
      pomoLeftMs = pomoLeft();
      pomoRunning = false;
    } else {
      pomoStart();
    }
  }
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) pomoReset();
  if (!pomoRunning && !pomoOnBreak) {
    int d = pycamera.justPressed(AWEXP_BUTTON_UP) ? 5 : pycamera.justPressed(AWEXP_BUTTON_DOWN) ? -5 : 0;
    if (d) {
      pomoWorkMin = constrain(pomoWorkMin + d, 5, 60);
      pomoLeftMs = pomoWorkMin * 60000UL;
    }
  }
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 100) return;
  lastDraw = millis();
  drawPomodoro();
}

// ---- Spirit level: bubble when lying flat, horizon line when held upright ----
float levelG[3] = {0, 0, 1};
float levelZero[2] = {0, 0};
bool wasLevel = false;
void levelTool() {
  float g[3];
  if (readAccelG(g))
    for (int i = 0; i < 3; i++) levelG[i] += (g[i] - levelG[i]) * 0.2f; // smooth out jitter
  bool flat = fabsf(levelG[2]) > fabsf(levelG[0]) && fabsf(levelG[2]) > fabsf(levelG[1]);
  float a1, a2 = 0;
  if (flat) {
    a1 = atan2f(levelG[0], fabsf(levelG[2])) * RAD_TO_DEG;
    a2 = atan2f(levelG[1], fabsf(levelG[2])) * RAD_TO_DEG;
  } else {
    a1 = atan2f(levelG[0], levelG[1]) * RAD_TO_DEG; // 0 when upright either way up
    if (a1 > 90) a1 -= 180;
    if (a1 < -90) a1 += 180;
  }
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) { // treat the current angle as level
    levelZero[0] = a1;
    levelZero[1] = a2;
  }
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) levelZero[0] = levelZero[1] = 0;
  a1 -= levelZero[0];
  a2 -= levelZero[1];
  bool level = fabsf(a1) < 0.7f && fabsf(a2) < 0.7f;
  if (level && !wasLevel) pycamera.speaker_tone(1568, 30);
  wasLevel = level;
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 50) return;
  lastDraw = millis();
  uint16_t col = level ? CLAUDE_GREEN : CLAUDE_ORANGE;
  ui->fillScreen(CLAUDE_DARK);
  const int cx = 120, cy = 104;
  char buf[32];
  if (flat) {
    ui->drawCircle(cx, cy, 72, CLAUDE_TRACK);
    ui->drawCircle(cx, cy, 36, CLAUDE_TRACK);
    ui->drawCircle(cx, cy, 14, CLAUDE_CREAM);
    ui->drawFastHLine(cx - 72, cy, 144, CLAUDE_TRACK);
    ui->drawFastVLine(cx, cy - 72, 144, CLAUDE_TRACK);
    int bx = cx - constrain((int)(a1 * 4), -58, 58), by = cy + constrain((int)(a2 * 4), -58, 58);
    ui->fillCircle(bx, by, 12, col);
    ui->fillCircle(bx - 4, by - 4, 3, CLAUDE_CREAM);
    snprintf(buf, sizeof(buf), "X %.1f   Y %.1f", a1, a2);
  } else {
    float r = -a1 * DEG_TO_RAD;
    ui->drawFastHLine(20, cy, 200, CLAUDE_TRACK);
    for (int k = -2; k <= 2; k++) // thick tilted line
      ui->drawLine(cx - cosf(r) * 100, cy - sinf(r) * 100 + k, cx + cosf(r) * 100, cy + sinf(r) * 100 + k, col);
    ui->fillCircle(cx, cy, 6, col);
    snprintf(buf, sizeof(buf), "%.1f", a1);
  }
  drawCentered(buf, 186, flat ? 2 : 4, col);
  if (level) drawCentered("LEVEL!", 22, 2, CLAUDE_GREEN);
  drawCentered("degrees", flat ? 204 : 220, 1, CLAUDE_GRAY);
  drawCentered("OK set zero   < clear zero", 230, 1, CLAUDE_GRAY);
  pushUi();
}

// ---- Photo booth: 3-2-1 countdown, full-size photo to the SD card ----
unsigned long boothStart = 0, boothMsgUntil = 0;
int boothShown = 0;
String boothMsg;
void boothTool() {
  pycamera.captureFrame();
  PyCameraFB *fb = pycamera.fb;
  drawCameraTitle(fb, "Photo Booth");
  if (boothStart) {
    int n = 3 - (int)((millis() - boothStart) / 1000);
    if (n <= 0) {
      pycamera.blitFrame(); // hand the preview frame back before the big photo
      boothStart = 0;
      pycamera.fillScreen(CLAUDE_CREAM); // flash
      pycamera.speaker_tone(1800, 40);
      bool ok = pycamera.takePhoto("BOOTH", FRAMESIZE_UXGA);
      boothMsg = ok ? "Saved to the SD card!" : pycamera.SDdetected() ? "Couldn't save the photo" : "No SD card";
      boothMsgUntil = millis() + 2500;
      if (ok) petBoost(3);
      return;
    }
    if (n != boothShown) {
      boothShown = n;
      pycamera.speaker_tone(n == 1 ? 1320 : 880, 60);
    }
    fb->setTextSize(10);
    fb->setTextColor(CLAUDE_ORANGE);
    fb->setCursor(92, 76);
    fb->print(n);
    drawBot(fb, 182, 186, 5, FACE_HAPPY, (millis() / 200) % 2 ? 2 : 1, (millis() / 200) % 2 ? 1 : 2);
  } else if (pycamera.justPressed(SHUTTER_BUTTON) || pycamera.justPressed(AWEXP_BUTTON_OK)) {
    boothStart = millis();
    boothShown = 0;
  }
  if (millis() < boothMsgUntil) drawCameraHint(fb, boothMsg, CLAUDE_ORANGE);
  else drawCameraHint(fb, "[Shutter] 3-2-1 photo", CLAUDE_CREAM);
  pycamera.blitFrame();
}

// ---- Timelapse: a frame every few seconds to the SD card, then a flipbook ----
#define TL_DIR "/timelapse"
const int TL_INTERVALS[] = {1, 2, 5, 10, 30, 60};
int tlIntervalIdx = 2, tlFrames = 0, tlState = 0; // state: 0 ready, 1 recording, 2 playing
int tlPlayFrame = 0;
unsigned long tlLastShot = 0, tlPlayAt = 0;
String tlMsg;
String tlPath(int i) {
  char p[32];
  snprintf(p, sizeof(p), TL_DIR "/tl_%04d.jpg", i);
  return p;
}
void tlCountFrames() {
  tlFrames = 0;
  while (tlFrames < 9999 && pycamera.sd.exists(tlPath(tlFrames).c_str())) tlFrames++;
}
void tlClear() {
  for (int i = 0; i < tlFrames; i++) pycamera.sd.remove(tlPath(i).c_str());
  tlFrames = 0;
}
bool tlSaveFrame() { // the preview frame is already a 240x240 JPEG: write it as-is
  if (!pycamera.frame) return false;
  File f = pycamera.sd.open(tlPath(tlFrames).c_str(), FILE_WRITE);
  if (!f) return false;
  bool ok = f.write(pycamera.frame->buf, pycamera.frame->len) == pycamera.frame->len;
  f.close();
  if (ok) tlFrames++;
  return ok;
}
bool jpegToUi(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  ui->drawRGBBitmap(x, y, bitmap, w, h);
  return true;
}
bool tlShowFrame(int i) {
  File f = pycamera.sd.open(tlPath(i).c_str(), FILE_READ);
  if (!f) return false;
  size_t len = f.size();
  uint8_t *buf = (uint8_t *)ps_malloc(len);
  if (!buf) {
    f.close();
    return false;
  }
  f.read(buf, len);
  f.close();
  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpegToUi); // captureFrame() sets its own callback back each time
  TJpgDec.drawJpg(0, 0, buf, len);
  free(buf);
  return true;
}
void timelapseStart() {
  tlState = 0;
  tlMsg = "";
  if (!pycamera.sd.exists(TL_DIR)) pycamera.sd.mkdir(TL_DIR);
  tlCountFrames();
}
void timelapseTool() {
  if (tlState == 2) { // flipbook playback at 10 fps
    if (pycamera.justPressed(AWEXP_BUTTON_OK) || pycamera.justPressed(AWEXP_BUTTON_LEFT) || pycamera.justPressed(SHUTTER_BUTTON)) {
      tlState = 0;
      return;
    }
    if (millis() - tlPlayAt < 100) return;
    tlPlayAt = millis();
    if (!tlShowFrame(tlPlayFrame)) {
      tlState = 0;
      tlMsg = "Couldn't read the frames";
      return;
    }
    ui->fillRoundRect(170, 4, 66, 14, 4, CLAUDE_DARK);
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_CREAM);
    ui->setCursor(174, 7);
    ui->printf("%d/%d", tlPlayFrame + 1, tlFrames);
    pushUi();
    tlPlayFrame = (tlPlayFrame + 1) % tlFrames;
    return;
  }
  pycamera.captureFrame();
  if (tlState == 1 && (!tlLastShot || millis() - tlLastShot >= TL_INTERVALS[tlIntervalIdx] * 1000UL)) {
    tlLastShot = millis();
    if (!tlSaveFrame()) {
      tlState = 0;
      tlMsg = pycamera.SDdetected() ? "SD card write failed" : "No SD card";
    }
  }
  PyCameraFB *fb = pycamera.fb;
  drawCameraTitle(fb, "Timelapse");
  fb->setCursor(150, 5);
  if (tlState == 1) {
    if ((millis() / 500) % 2) fb->fillCircle(154, 8, 4, CLAUDE_RED);
    fb->setCursor(162, 5);
    fb->setTextColor(CLAUDE_RED);
    fb->printf("REC %d", tlFrames);
  } else {
    fb->setTextColor(CLAUDE_GRAY);
    fb->printf("%d frames", tlFrames);
  }
  char hint[48];
  if (tlMsg.length()) drawCameraHint(fb, tlMsg, CLAUDE_RED);
  else if (tlState == 1) {
    snprintf(hint, sizeof(hint), "Every %ds   OK stop", TL_INTERVALS[tlIntervalIdx]);
    drawCameraHint(fb, hint, CLAUDE_CREAM);
  } else {
    snprintf(hint, sizeof(hint), "OK rec  UP/DN every %ds  < play", TL_INTERVALS[tlIntervalIdx]);
    drawCameraHint(fb, hint, CLAUDE_CREAM);
  }
  pycamera.blitFrame();
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
    tlMsg = "";
    if (tlState == 1) {
      tlState = 0;
    } else { // a new recording replaces the old one
      tlClear();
      tlState = 1;
      tlLastShot = 0;
    }
  }
  if (tlState == 0) {
    if (pycamera.justPressed(AWEXP_BUTTON_UP)) tlIntervalIdx = min(tlIntervalIdx + 1, 5);
    if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) tlIntervalIdx = max(tlIntervalIdx - 1, 0);
    if (pycamera.justPressed(AWEXP_BUTTON_LEFT) && tlFrames > 1) {
      tlState = 2;
      tlPlayFrame = 0;
    }
  }
}

// ---- QR scanner: frames go to the phone app, ML Kit reads the code ----
String qrText;
unsigned long qrAt = 0, lastQrSend = 0;
int16_t qrPts[8];
bool qrHasPts = false, qrBridgeDown = false;
void handleQrResult(bool parsed, JsonDocument &doc) {
  qrBridgeDown = !parsed;
  if (!parsed) return;
  String text = (const char *)(doc["text"] | "");
  if (!text.length()) return;
  if (text != qrText) pycamera.speaker_tone(1568, 40);
  qrText = text;
  qrAt = millis();
  JsonArray pts = doc["points"];
  qrHasPts = pts.size() == 8;
  for (int i = 0; i < 8 && qrHasPts; i++) qrPts[i] = pts[i] | 0;
}
void qrTool() {
  pycamera.captureFrame();
  if (!netBusy() && pycamera.frame && millis() - lastQrSend > 200) {
    size_t len = pycamera.frame->len;
    uint8_t *jpg = (uint8_t *)ps_malloc(len);
    if (jpg) {
      memcpy(jpg, pycamera.frame->buf, len);
      if (startNetJob(JOB_QR, "/qr", jpg, len, "image/jpeg")) lastQrSend = millis();
      else free(jpg);
    }
  }
  PyCameraFB *fb = pycamera.fb;
  // viewfinder corners and a sweeping scan line
  const int a = 40, b = 200, L = 22;
  for (int k = 0; k < 3; k++) {
    fb->drawFastHLine(a, a + k, L, CLAUDE_ORANGE);
    fb->drawFastVLine(a + k, a, L, CLAUDE_ORANGE);
    fb->drawFastHLine(b - L, a + k, L, CLAUDE_ORANGE);
    fb->drawFastVLine(b - k, a, L, CLAUDE_ORANGE);
    fb->drawFastHLine(a, b - k, L, CLAUDE_ORANGE);
    fb->drawFastVLine(a + k, b - L, L, CLAUDE_ORANGE);
    fb->drawFastHLine(b - L, b - k, L, CLAUDE_ORANGE);
    fb->drawFastVLine(b - k, b - L, L, CLAUDE_ORANGE);
  }
  fb->drawFastHLine(a + 6, a + (millis() / 6) % (b - a), b - a - 12, CLAUDE_ORANGE);
  if (qrHasPts && millis() - qrAt < 800)
    for (int i = 0; i < 4; i++) {
      int j = (i + 1) % 4;
      fb->drawLine(qrPts[i * 2], qrPts[i * 2 + 1], qrPts[j * 2], qrPts[j * 2 + 1], CLAUDE_GREEN);
    }
  drawCameraTitle(fb, qrBridgeDown ? "QR Scanner - phone offline" : "QR Scanner");
  if (qrText.length()) {
    String kind = "Text", body = qrText;
    if (qrText.startsWith("http")) kind = "Link";
    else if (qrText.startsWith("WIFI:")) { // WIFI:T:WPA;S:name;P:pass;;
      int s = qrText.indexOf("S:");
      kind = "WiFi network";
      body = s >= 0 ? qrText.substring(s + 2, qrText.indexOf(';', s)) : qrText;
    }
    fb->fillRect(0, 166, 240, 74, CLAUDE_DARK);
    fb->setTextSize(1);
    fb->setTextColor(CLAUDE_ORANGE);
    fb->setCursor(6, 170);
    fb->print(kind);
    String lines[4];
    int n = wrapText(body, 38, lines, 4);
    fb->setTextColor(CLAUDE_CREAM);
    for (int i = 0; i < n; i++) {
      fb->setCursor(6, 182 + i * 11);
      fb->print(lines[i]);
    }
    fb->setTextColor(CLAUDE_GRAY);
    fb->setCursor(186, 170);
    fb->print("OK clear");
  } else {
    drawCameraHint(fb, "Hold a QR code inside the frame", CLAUDE_CREAM);
  }
  pycamera.blitFrame();
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) qrText = "";
}

// ---- Home controls: the light/fan controller and the two robot Arduinos ----
const char *HOME_TILES[] = {"Light", "Fan", "Robot dance", "Robot stop", "Robot sleep", "Robot wake"};
bool lightOn = false, fanOn = false; // last thing we told them (they don't report back)
int homeSel = 0;
String homeToast;
unsigned long homeToastUntil = 0;
void homeAction(int i) {
  ui->fillRoundRect(60, 100, 120, 36, 10, CLAUDE_ORANGE);
  drawCentered("Sending...", 114, 1, CLAUDE_DARK);
  pushUi();
  switch (i) {
    case 0:
      lightOn = !lightOn;
      sendControlCommand(lightOn ? "light_on" : "light_off");
      break;
    case 1:
      fanOn = !fanOn;
      sendControlCommand(fanOn ? "fan_on" : "fan_off");
      break;
    case 2:
      sendDanceCommand();
      sendI2CCommandTo(ARDUINO2_ADDR, "exp1");
      break;
    case 3:
      sendI2CCommandTo(ARDUINO_ADDR, "stop");
      sendI2CCommandTo(ARDUINO2_ADDR, "stop");
      break;
    case 4:
      sendI2CCommandTo(ARDUINO_ADDR, "sleep");
      sendI2CCommandTo(ARDUINO2_ADDR, "sleep");
      break;
    case 5:
      sendI2CCommandTo(ARDUINO_ADDR, "wakeup");
      sendI2CCommandTo(ARDUINO2_ADDR, "wake");
      break;
  }
  homeToast = String(HOME_TILES[i]) + (i == 0 ? (lightOn ? " on" : " off") : i == 1 ? (fanOn ? " on" : " off") : " sent");
  homeToastUntil = millis() + 1500;
}
void homeTool() {
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT) || pycamera.justPressed(AWEXP_BUTTON_RIGHT)) homeSel ^= 1;
  if (pycamera.justPressed(AWEXP_BUTTON_UP)) homeSel = (homeSel + 4) % 6;
  if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) homeSel = (homeSel + 2) % 6;
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) homeAction(homeSel);
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 50) return;
  lastDraw = millis();
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print("Home");
  for (int i = 0; i < 6; i++) {
    int x = 8 + (i % 2) * 116, y = 36 + (i / 2) * 60;
    bool on = (i == 0 && lightOn) || (i == 1 && fanOn);
    ui->fillRoundRect(x, y, 108, 54, 10, i == homeSel ? 0x2945 : 0x2104);
    ui->drawRoundRect(x, y, 108, 54, 10, i == homeSel ? CLAUDE_ORANGE : CLAUDE_TRACK);
    int ix = x + 22, iy = y + 24;
    if (i == 0) { // bulb
      ui->fillCircle(ix, iy - 3, 9, on ? CLAUDE_AMBER : CLAUDE_GRAY);
      ui->fillRect(ix - 4, iy + 6, 9, 6, CLAUDE_CREAM);
    } else if (i == 1) { // fan blades, spinning when on
      float spin = on ? millis() / 80.0f : 0;
      for (int k = 0; k < 3; k++) {
        float a = spin + k * 2.094f;
        ui->fillCircle(ix + cosf(a) * 7, iy + sinf(a) * 7, 5, on ? CLAUDE_BLUE : CLAUDE_GRAY);
      }
      ui->fillCircle(ix, iy, 3, CLAUDE_CREAM);
    } else { // little robot
      drawBot(ui, ix - 10, iy - 6, 3, i == 4 ? FACE_BLINK : FACE_HAPPY, i == 2 ? 2 : 0, i == 2 ? 2 : 0);
    }
    String label = HOME_TILES[i];
    int sp = label.indexOf(' ');
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_CREAM);
    ui->setCursor(x + 44, y + (sp > 0 ? 16 : 22));
    ui->print(sp > 0 ? label.substring(0, sp) : label);
    if (sp > 0) {
      ui->setCursor(x + 44, y + 28);
      ui->print(label.substring(sp + 1));
    }
    if (i < 2) {
      ui->setTextColor(on ? CLAUDE_AMBER : CLAUDE_GRAY);
      ui->setCursor(x + 44, y + 34);
      ui->print(on ? "ON" : "OFF");
    }
  }
  if (millis() < homeToastUntil) drawCentered(homeToast, 218, 1, CLAUDE_ORANGE);
  else drawCentered("D-pad pick   OK press", 226, 1, CLAUDE_GRAY);
  pushUi();
}

// ---- Phone remote: QR code to open the board's own web page (or join the WiFi) ----
int qrDrawX, qrDrawY, qrDrawMax;
bool remoteShowWifi = false, remoteDrawn = false;
void drawQrModules(esp_qrcode_handle_t q) {
  int size = esp_qrcode_get_size(q);
  int scale = max(1, qrDrawMax / (size + 4));
  int full = (size + 4) * scale; // 2-module white border
  int px = qrDrawX - full / 2, py = qrDrawY - full / 2;
  ui->fillRect(px, py, full, full, 0xFFFF);
  for (int y = 0; y < size; y++)
    for (int x = 0; x < size; x++)
      if (esp_qrcode_get_module(q, x, y)) ui->fillRect(px + (x + 2) * scale, py + (y + 2) * scale, scale, scale, 0x0000);
}
void drawQrCode(const String &text, int cx, int cy, int maxPx) {
  qrDrawX = cx;
  qrDrawY = cy;
  qrDrawMax = maxPx;
  esp_qrcode_config_t cfg = {drawQrModules, 10, ESP_QRCODE_ECC_LOW};
  esp_qrcode_generate(&cfg, text.c_str());
}
void remoteTool() {
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT) || pycamera.justPressed(AWEXP_BUTTON_RIGHT)) {
    remoteShowWifi = !remoteShowWifi;
    remoteDrawn = false;
  }
  static wl_status_t lastWifi = WL_IDLE_STATUS;
  if (WiFi.status() != lastWifi) {
    lastWifi = WiFi.status();
    remoteDrawn = false;
  }
  if (remoteDrawn) return; // a static screen: only redraw when something changes
  remoteDrawn = true;
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print(remoteShowWifi ? "Join WiFi" : "Phone Remote");
  if (WiFi.status() != WL_CONNECTED) {
    drawCentered("Waiting for WiFi...", 110, 1, CLAUDE_GRAY);
  } else if (remoteShowWifi) {
    drawQrCode("WIFI:T:WPA;S:" + String(ssid) + ";P:" + String(password) + ";;", 120, 112, 160);
    drawCentered("Scan to join " + String(ssid), 200, 1, CLAUDE_CREAM);
  } else {
    String url = "http://" + WiFi.localIP().toString() + "/";
    drawQrCode(url, 120, 112, 160);
    drawCentered(url, 198, 1, CLAUDE_CREAM);
    drawCentered("or http://clawd.local", 210, 1, CLAUDE_GRAY);
  }
  drawCentered("</> remote / join WiFi", 228, 1, CLAUDE_GRAY);
  pushUi();
}

void startTool(int t) {
  currentTool = t;
  currentMode = MODE_TOOL;
  if (t == TOOL_TIMELAPSE) timelapseStart();
  if (t == TOOL_BOOTH) boothStart = 0;
  if (t == TOOL_QR) qrText = "";
  if (t == TOOL_REMOTE) remoteDrawn = false;
  if (t == TOOL_HUNT && huntState == 4) huntState = 3;
}
void toolsMenuMode() {
  if (pycamera.justPressed(AWEXP_BUTTON_UP)) toolSelection = (toolSelection + TOOL_COUNT - 1) % TOOL_COUNT;
  if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) toolSelection = (toolSelection + 1) % TOOL_COUNT;
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
    startTool(toolSelection);
    return;
  }
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 100) return;
  lastDraw = millis();
  drawListScreen("Toolbox", TOOL_NAMES, TOOL_COUNT, toolSelection, TOOL_HINTS[toolSelection]);
}
void toolMode() {
  switch (currentTool) {
    case TOOL_POMODORO: pomodoroTool(); break;
    case TOOL_LEVEL: levelTool(); break;
    case TOOL_BOOTH: boothTool(); break;
    case TOOL_TIMELAPSE: timelapseTool(); break;
    case TOOL_QR: qrTool(); break;
    case TOOL_HOME: homeTool(); break;
    case TOOL_REMOTE: remoteTool(); break;
    case TOOL_AI: aiTool(); break;
    case TOOL_HUNT: huntTool(); break;
  }
}

// ===================== WEB REMOTE =====================
// The board serves a small page on your WiFi: open http://clawd.local (or scan the QR code in
// Toolbox > Phone Remote) to see usage, edit alarms, feed Clawd, start a dance or a focus timer.
// A tiny hand-rolled HTTP server: the ESP32 WebServer library pulls in FS.h, which stops the
// SD card library from defining File and breaks the camera library's takePhoto().
WiFiServer web(80);
bool webStarted = false;
const char REMOTE_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Clawd remote</title><style>
body{font-family:system-ui,sans-serif;background:#141413;color:#faf9f5;margin:0 auto;padding:16px;max-width:520px}
h1{color:#d97757;font-size:22px;margin:0 0 12px}h2{font-size:14px;color:#8a8a8a;margin:18px 0 8px}
.card{background:#22211f;border-radius:12px;padding:12px}
.bar{height:10px;background:#2e2d2a;border-radius:5px;overflow:hidden;margin:4px 0 10px}.bar i{display:block;height:100%;background:#d97757}
button{background:#d97757;color:#141413;border:0;border-radius:10px;padding:10px 12px;font-weight:600;margin:3px;font-size:14px}
button.g{background:#2e2d2a;color:#faf9f5}.row{display:flex;gap:6px;align-items:center;margin:6px 0;flex-wrap:wrap}
input,select{background:#2e2d2a;color:#faf9f5;border:0;border-radius:8px;padding:8px;font-size:14px}
</style></head><body><h1>Clawd remote</h1><div class=card id=st>Loading...</div>
<h2>Do something</h2><div class=card>
<button onclick="act('dance')">Dance</button><button class=g onclick="act('stop')">Stop</button>
<button onclick="act('ring')">Test alarm</button><button onclick="act('feed')">Feed Clawd</button>
<button onclick="act('pomodoro')">Start focus</button><br>
<button class=g onclick="act('light_on')">Light on</button><button class=g onclick="act('light_off')">Light off</button>
<button class=g onclick="act('fan_on')">Fan on</button><button class=g onclick="act('fan_off')">Fan off</button></div>
<h2>Alarms</h2><div class=card id=al></div>
<script>
const R=['Once','Every day','Weekdays','Weekends'],$=id=>document.getElementById(id),p=n=>String(n).padStart(2,'0');
async function act(a){await fetch('/api/action?do='+a,{method:'POST'});load()}
async function save(i){const t=$('t'+i).value.split(':');
await fetch(`/api/alarm?i=${i}&h=${+t[0]}&m=${+t[1]}&r=${$('r'+i).value}&on=${$('o'+i).checked?1:0}`,{method:'POST'});load(true)}
async function load(force){const s=await(await fetch('/api/status')).json();
let h=`<b style="font-size:20px">${s.time}</b>${s.approx?' <small>(not synced yet)</small>':''}<br><small>Now showing: ${s.mode}</small><br><br>`;
if(s.phone)h+=`Phone link: ${s.phone.online?'connected':'lost'}${s.phone.battery>=0?' &middot; battery '+s.phone.battery+'%':''} &middot; ${s.phone.requests} Claude requests today<br><br>`;
h+=`Clawd: food ${s.pet.food}% &middot; energy ${s.pet.energy}% &middot; fun ${s.pet.fun}%${s.pet.nap?' &middot; napping':''}`;
$('st').innerHTML=h;const f=document.activeElement;
if(force||!f||!['INPUT','SELECT'].includes(f.tagName))$('al').innerHTML=s.alarms.map((a,i)=>`<div class=row>
<input type=time id=t${i} value="${p(a.h)}:${p(a.m)}"><select id=r${i}>${R.map((n,j)=>`<option value=${j}${j==a.r?' selected':''}>${n}</option>`).join('')}</select>
<label><input type=checkbox id=o${i}${a.on?' checked':''}> on</label><button onclick="save(${i})">Save</button></div>`).join('')}
load(true);setInterval(load,5000)
</script></body></html>)HTML";
const char *modeName() {
  switch (currentMode) {
    case MODE_USAGE: return "API Status";
    case MODE_TALK: return "Clawd Talk";
    case MODE_ASK: return "Claude Ask";
    case MODE_DANCE: return "Dance";
    case MODE_PET: return "Clawd Pet";
    case MODE_ALARMS: return "Alarms";
    case MODE_ALARM_RING: return "Alarm ringing!";
    case MODE_FACE: return "Face Finder";
    case MODE_WORLD: return feedMode ? "Daily Feed" : "World Live";
    case MODE_GAME:
    case MODE_GAME_MENU: return "Games";
    case MODE_TOOL:
    case MODE_TOOLS: return "Toolbox";
    case MODE_SLEEP: return "Sleeping (clock)";
    case MODE_HOME_ASSISTANT: return "Home Assistant";
    case MODE_RETRO: return "Retro Cam";
    case MODE_NOWPLAY: return "Now Playing";
    case MODE_BROWSER: return "Browser";
    case MODE_MUSIC: return "Music";
    case MODE_FRIENDS: return "Friends";
    default: return "Menu";
  }
}
// Sends one complete HTTP response
void webReply(WiFiClient &c, int code, const char *type, const char *body) {
  c.printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
           "Connection: close\r\n\r\n",
           code, code == 200 ? "OK" : "Bad Request", type, (unsigned)strlen(body));
  c.print(body);
}
// Value of name=... in a query string like "i=0&h=7" ("" if missing)
String queryArg(const String &query, const char *name) {
  String q = "&" + query, key = "&" + String(name) + "=";
  int i = q.indexOf(key);
  if (i < 0) return "";
  i += key.length();
  int end = q.indexOf('&', i);
  return q.substring(i, end < 0 ? q.length() : end);
}
void webStatus(WiFiClient &c) {
  JsonDocument doc;
  struct tm t;
  doc["time"] = localNow(&t) ? clockText(t.tm_hour, t.tm_min) : String("--:--");
  doc["approx"] = (bool)timeApprox;
  doc["mode"] = modeName();
  if (usage.valid) {
    doc["phone"]["online"] = phoneOnline();
    doc["phone"]["battery"] = usage.battery;
    doc["phone"]["requests"] = usage.requests;
  }
  doc["pet"]["food"] = (int)pet.food;
  doc["pet"]["energy"] = (int)pet.energy;
  doc["pet"]["fun"] = (int)pet.fun;
  doc["pet"]["nap"] = pet.napping;
  for (int i = 0; i < MAX_ALARMS; i++) {
    JsonObject a = doc["alarms"].add<JsonObject>();
    a["on"] = alarms[i].on;
    a["h"] = alarms[i].hour;
    a["m"] = alarms[i].minute;
    a["r"] = alarms[i].repeat;
  }
  String out;
  serializeJson(doc, out);
  webReply(c, 200, "application/json", out.c_str());
}
void webAlarm(WiFiClient &c, const String &q) {
  int i = queryArg(q, "i").toInt();
  if (i < 0 || i >= MAX_ALARMS || !queryArg(q, "h").length()) {
    webReply(c, 400, "text/plain", "bad alarm");
    return;
  }
  Alarm &a = alarms[i];
  a.hour = constrain(queryArg(q, "h").toInt(), 0, 23);
  a.minute = constrain(queryArg(q, "m").toInt(), 0, 59);
  a.repeat = constrain(queryArg(q, "r").toInt(), 0, 3);
  a.on = queryArg(q, "on") == "1";
  saveAlarms();
  webReply(c, 200, "text/plain", "ok");
}
void webAction(WiFiClient &c, const String &q) {
  String a = queryArg(q, "do");
  if (currentMode == MODE_SLEEP && a != "feed") wakeUp();
  if (a == "dance") {
    if (currentMode == MODE_ALARM_RING) endAlarm(false);
    currentMode = MODE_DANCE;
    startDance(0);
  } else if (a == "stop") {
    if (currentMode == MODE_ALARM_RING) endAlarm(false);
    else if (currentMode == MODE_DANCE) {
      stopDance();
      currentMode = MODE_USAGE;
    }
  } else if (a == "ring") {
    ringAlarm();
  } else if (a == "feed") {
    pet.food = min(100.0f, pet.food + 25);
    savePet();
  } else if (a == "pomodoro") {
    pomoReset();
    pomoStart();
    stopDance();
    startTool(TOOL_POMODORO);
  } else if (a == "light_on" || a == "light_off" || a == "fan_on" || a == "fan_off") {
    if (a.startsWith("light")) lightOn = a == "light_on";
    else fanOn = a == "fan_on";
    sendControlCommand(a);
  } else {
    webReply(c, 400, "text/plain", "unknown action");
    return;
  }
  lastActivity = millis();
  webReply(c, 200, "text/plain", "ok");
}
// Called every loop(): starts the server once WiFi is up, then answers one request at a time
void webTick() {
  if (!webStarted) {
    if (WiFi.status() != WL_CONNECTED) return;
    web.begin();
    if (MDNS.begin("clawd")) MDNS.addService("http", "tcp", 80);
    webStarted = true;
    Serial.println("Remote: http://" + WiFi.localIP().toString() + "/  or  http://clawd.local/");
  }
  WiFiClient c = web.accept();
  if (!c) return;
  String request = c.readStringUntil('\n'); // e.g. "POST /api/alarm?i=0&h=7 HTTP/1.1"
  while (c.connected()) { // skip the headers (all our parameters are in the URL)
    String header = c.readStringUntil('\n');
    if (header.length() <= 1) break;
  }
  int sp1 = request.indexOf(' '), sp2 = request.indexOf(' ', sp1 + 1);
  String url = request.substring(sp1 + 1, sp2), path = url, query;
  int qm = url.indexOf('?');
  if (qm >= 0) {
    path = url.substring(0, qm);
    query = url.substring(qm + 1);
  }
  if (path == "/") webReply(c, 200, "text/html", REMOTE_PAGE);
  else if (path == "/api/status") webStatus(c);
  else if (path == "/api/alarm") webAlarm(c, query);
  else if (path == "/api/action") webAction(c, query);
  else if (webExtraRoute(c, path, query)) {} // Retro Cam gallery, hunt codes
  else webReply(c, 404, "text/plain", "not found");
  c.stop();
}

// ===================== FUN LAB =====================
// Retro Cam, Now Playing, Browser, Music, Friends, Clawd Beats, Pong Duel, Person Detector
// and the Scavenger Hunt. Everything here is extra: nothing above depends on it.
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
// 0-255 brightness of an RGB565 pixel
uint8_t lum565(uint16_t c) {
  return (uint8_t)((((c >> 11) & 0x1F) * 8 * 77 + ((c >> 5) & 0x3F) * 4 * 150 + (c & 0x1F) * 8 * 29) >> 8);
}
// First unused name like /retrocam/RC0007.jpg
String sdNextName(const char *dir, const char *prefix, const char *ext) {
  if (!pycamera.sd.exists(dir)) pycamera.sd.mkdir(dir);
  char p[48];
  for (int i = 1; i < 10000; i++) {
    snprintf(p, sizeof(p), "%s/%s%04d.%s", dir, prefix, i, ext);
    if (!pycamera.sd.exists(p)) return p;
  }
  return "";
}
// Saves a 240x240 RGB565 screen image to the SD card as a JPEG
bool saveScreenJpeg(const uint16_t *px, const String &path) {
  uint16_t *be = (uint16_t *)ps_malloc(240 * 240 * 2);
  if (!be) return false;
  for (int i = 0; i < 240 * 240; i++) be[i] = (px[i] >> 8) | (px[i] << 8); // the encoder wants camera byte order
  uint8_t *jpg = NULL;
  size_t len = 0;
  bool ok = fmt2jpg((uint8_t *)be, 240 * 240 * 2, 240, 240, PIXFORMAT_RGB565, 85, &jpg, &len);
  free(be);
  if (ok) {
    File f = pycamera.sd.open(path.c_str(), FILE_WRITE);
    ok = f && f.write(jpg, len) == len;
    f.close();
  }
  free(jpg);
  return ok;
}

// ---- Animated GIF writer (streams frames straight to the SD card) ----
File gifFile;
uint8_t gifBlock[255];
int gifBlockLen = 0, gifAccBits = 0, gifMinCode = 8;
uint32_t gifAcc = 0;
#define GIF_HASH 8191
uint32_t *gifKeys = NULL; // LZW dictionary: (prefix code << 8 | pixel) -> code
int16_t *gifCodes = NULL;
bool gifBegin(const String &path, const uint16_t *pal, int ncolors) {
  int bits = 2;
  while ((1 << bits) < ncolors) bits++;
  gifMinCode = bits;
  if (!gifKeys) gifKeys = (uint32_t *)ps_malloc(GIF_HASH * sizeof(uint32_t));
  if (!gifCodes) gifCodes = (int16_t *)ps_malloc(GIF_HASH * sizeof(int16_t));
  if (!gifKeys || !gifCodes) return false;
  gifFile = pycamera.sd.open(path.c_str(), FILE_WRITE);
  if (!gifFile) return false;
  uint8_t hdr[13] = {'G', 'I', 'F', '8', '9', 'a', 240, 0, 240, 0, (uint8_t)(0x80 | ((bits - 1) << 4) | (bits - 1)), 0, 0};
  gifFile.write(hdr, 13);
  for (int i = 0; i < (1 << bits); i++) { // global colour table
    uint16_t c = i < ncolors ? pal[i] : 0;
    uint8_t rgb[3] = {(uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC), (uint8_t)((c << 3) & 0xF8)};
    gifFile.write(rgb, 3);
  }
  static const uint8_t LOOP[19] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
  gifFile.write(LOOP, 19); // loop forever
  return true;
}
void gifByte(uint8_t b) {
  gifBlock[gifBlockLen++] = b;
  if (gifBlockLen == 255) {
    gifFile.write((uint8_t)255);
    gifFile.write(gifBlock, 255);
    gifBlockLen = 0;
  }
}
void gifCode(int code, int size) {
  gifAcc |= (uint32_t)code << gifAccBits;
  gifAccBits += size;
  while (gifAccBits >= 8) {
    gifByte(gifAcc & 0xFF);
    gifAcc >>= 8;
    gifAccBits -= 8;
  }
}
// One 240x240 frame of palette indices, shown for delayCs hundredths of a second
void gifFrame(const uint8_t *px, int delayCs) {
  uint8_t gce[8] = {0x21, 0xF9, 0x04, 0x00, (uint8_t)(delayCs & 0xFF), (uint8_t)(delayCs >> 8), 0x00, 0x00};
  gifFile.write(gce, 8);
  uint8_t desc[10] = {0x2C, 0, 0, 0, 0, 240, 0, 240, 0, 0x00};
  gifFile.write(desc, 10);
  gifFile.write((uint8_t)gifMinCode);
  const int clear = 1 << gifMinCode;
  int codeSize = gifMinCode + 1, maxCode = clear + 1;
  memset(gifKeys, 0xFF, GIF_HASH * sizeof(uint32_t));
  gifAcc = 0;
  gifAccBits = 0;
  gifBlockLen = 0;
  gifCode(clear, codeSize);
  int cur = px[0];
  for (int i = 1; i < 240 * 240; i++) { // LZW, the same scheme as the classic gif.h
    uint8_t c = px[i];
    uint32_t key = ((uint32_t)cur << 8) | c;
    uint32_t h = (key * 2654435761u) % GIF_HASH;
    while (gifKeys[h] != 0xFFFFFFFF && gifKeys[h] != key) h = (h + 1) % GIF_HASH;
    if (gifKeys[h] == key) {
      cur = gifCodes[h];
      continue;
    }
    gifCode(cur, codeSize);
    gifKeys[h] = key;
    gifCodes[h] = ++maxCode;
    if (maxCode >= (1 << codeSize)) codeSize++;
    if (maxCode == 4095) { // dictionary full: start a fresh one
      gifCode(clear, codeSize);
      memset(gifKeys, 0xFF, GIF_HASH * sizeof(uint32_t));
      codeSize = gifMinCode + 1;
      maxCode = clear + 1;
    }
    cur = c;
  }
  gifCode(cur, codeSize);
  gifCode(clear, codeSize);
  gifCode(clear + 1, gifMinCode + 1);
  if (gifAccBits > 0) gifByte(gifAcc & 0xFF);
  if (gifBlockLen) {
    gifFile.write((uint8_t)gifBlockLen);
    gifFile.write(gifBlock, gifBlockLen);
  }
  gifFile.write((uint8_t)0);
}
void gifEnd() {
  gifFile.write((uint8_t)0x3B);
  gifFile.close();
}

// ---- Retro Cam: Game Boy Camera look, filters, photos and GIFs ----
#define RC_FILTERS 8
const char *RC_NAMES[] = {"Game Boy", "1-bit", "Pixel art", "ASCII", "Glitch", "Thermal", "Sketch", "Normal"};
const uint16_t RC_GB[4] = {RGB565(15, 56, 15), RGB565(48, 98, 48), RGB565(139, 172, 15), RGB565(155, 188, 15)};
const uint16_t RC_BIT[2] = {RGB565(20, 20, 19), RGB565(250, 249, 245)};
const uint16_t RC_ASCII[2] = {RGB565(8, 12, 8), RGB565(51, 255, 102)};
const uint16_t RC_PICO[16] = {RGB565(0, 0, 0),       RGB565(29, 43, 83),    RGB565(126, 37, 83),   RGB565(0, 135, 81),
                              RGB565(171, 82, 54),   RGB565(95, 87, 79),    RGB565(194, 195, 199), RGB565(255, 241, 232),
                              RGB565(255, 0, 77),    RGB565(255, 163, 0),   RGB565(255, 236, 39),  RGB565(0, 228, 54),
                              RGB565(41, 173, 255),  RGB565(131, 118, 156), RGB565(255, 119, 168), RGB565(255, 204, 170)};
const uint8_t BAYER4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
const char RC_RAMP[] = " .:-=+*#%@";
int rcFilter = 0;
uint8_t *rcIdx = NULL;  // palette index of every pixel, for GIFs
uint8_t *rcLum = NULL;  // brightness buffer for the sketch filter
uint16_t rcPal332[256]; // fixed 256-colour palette for full-colour GIFs
uint16_t rcThermal[256];
bool rcReady = false, rcShare = false, rcShareDrawn = false;
int rcGifFrames = 0;    // > 0 while a GIF is recording
unsigned long rcGifNext = 0, rcToastUntil = 0, rcFlashUntil = 0;
String rcLastFile, rcGifPath, rcToast;
bool retroInit() {
  if (rcReady) return true;
  rcIdx = (uint8_t *)ps_malloc(240 * 240);
  rcLum = (uint8_t *)ps_malloc(240 * 240);
  if (!rcIdx || !rcLum) return false;
  for (int i = 0; i < 256; i++) rcPal332[i] = RGB565((i >> 5) * 255 / 7, ((i >> 2) & 7) * 255 / 7, (i & 3) * 255 / 3);
  // thermal camera colours: black, blue, purple, red, orange, yellow, white
  const uint8_t stops[7][4] = {{0, 0, 0, 0}, {50, 20, 0, 120}, {100, 160, 0, 150}, {150, 255, 40, 0},
                               {200, 255, 170, 0}, {235, 255, 240, 60}, {255, 255, 255, 230}};
  for (int v = 0; v < 256; v++) {
    int k = 0;
    while (k < 5 && v > stops[k + 1][0]) k++;
    float t = (float)(v - stops[k][0]) / max(1, stops[k + 1][0] - stops[k][0]);
    rcThermal[v] = RGB565((int)(stops[k][1] + t * (stops[k + 1][1] - stops[k][1])),
                          (int)(stops[k][2] + t * (stops[k + 1][2] - stops[k][2])),
                          (int)(stops[k][3] + t * (stops[k + 1][3] - stops[k][3])));
  }
  rcReady = true;
  return true;
}
// Draws src through the current filter into dst. Returns the small palette it used (and its size),
// or NULL for full-colour filters, whose GIF frames use the fixed 256-colour palette.
const uint16_t *retroFilter(const uint16_t *src, uint16_t *dst, int *ncolors) {
  switch (rcFilter) {
    case 0:   // Game Boy: 4 greens with ordered dithering
    case 1: { // 1-bit: 2 tones with ordered dithering
      int levels = rcFilter == 0 ? 4 : 2;
      const uint16_t *pal = rcFilter == 0 ? RC_GB : RC_BIT;
      for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++) {
          int i = y * 240 + x;
          float l = constrain((lum565(src[i]) - 110) * 1.4f + 128, 0.0f, 255.0f); // punchier contrast
          float v = l / 255.0f * (levels - 1) + (BAYER4[(y & 3) * 4 + (x & 3)] / 16.0f - 0.47f);
          int q = constrain((int)(v + 0.5f), 0, levels - 1);
          rcIdx[i] = q;
          dst[i] = pal[q];
        }
      *ncolors = levels;
      return pal;
    }
    case 2: // pixel art: 6x6 blocks in the PICO-8 palette
      for (int by = 0; by < 240; by += 6)
        for (int bx = 0; bx < 240; bx += 6) {
          int r = 0, g = 0, b = 0;
          for (int y = by; y < by + 6; y++)
            for (int x = bx; x < bx + 6; x++) {
              uint16_t c = src[y * 240 + x];
              r += (c >> 8) & 0xF8;
              g += (c >> 3) & 0xFC;
              b += (c << 3) & 0xF8;
            }
          r /= 36;
          g /= 36;
          b /= 36;
          int best = 0;
          long bestD = LONG_MAX;
          for (int k = 0; k < 16; k++) {
            uint16_t p = RC_PICO[k];
            long dr = r - ((p >> 8) & 0xF8), dg = g - ((p >> 3) & 0xFC), db = b - ((p << 3) & 0xF8);
            long d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bestD) {
              bestD = d;
              best = k;
            }
          }
          for (int y = by; y < by + 6; y++)
            for (int x = bx; x < bx + 6; x++) {
              rcIdx[y * 240 + x] = best;
              dst[y * 240 + x] = RC_PICO[best];
            }
        }
      *ncolors = 16;
      return RC_PICO;
    case 3: { // ASCII art: one character per 6x8 cell, green-screen style
      for (int i = 0; i < 240 * 240; i++) dst[i] = RC_ASCII[0];
      for (int cy = 0; cy < 30; cy++)
        for (int cx = 0; cx < 40; cx++) {
          int sum = 0;
          for (int y = 0; y < 8; y += 2)
            for (int x = 0; x < 6; x += 2) sum += lum565(src[(cy * 8 + y) * 240 + cx * 6 + x]);
          char ch = RC_RAMP[min(9, sum / 12 * 10 / 256)];
          if (ch != ' ') ui->drawChar(cx * 6, cy * 8, ch, RC_ASCII[1], RC_ASCII[0], 1);
        }
      for (int i = 0; i < 240 * 240; i++) rcIdx[i] = dst[i] == RC_ASCII[1];
      *ncolors = 2;
      return RC_ASCII;
    }
    case 4: { // glitch: torn rows, split colour channels, scanlines
      uint32_t seed = millis() / 120;
      for (int y = 0; y < 240; y++) {
        uint32_t h = (y / 8 + seed) * 2654435761u;
        int shift = (h % 7 == 0) ? (int)(h >> 24) % 40 - 20 : 0;
        bool dim = y % 3 == 0;
        for (int x = 0; x < 240; x++) {
          int xr = constrain(x + shift + 4, 0, 239), xg = constrain(x + shift, 0, 239), xb = constrain(x + shift - 4, 0, 239);
          uint16_t r = src[y * 240 + xr] & 0xF800, g = src[y * 240 + xg] & 0x07E0, b = src[y * 240 + xb] & 0x001F;
          uint16_t c = r | g | b;
          if (dim) c = (c >> 1) & 0x7BEF; // half brightness
          dst[y * 240 + x] = c;
        }
      }
      break;
    }
    case 5: // thermal camera
      for (int i = 0; i < 240 * 240; i++) dst[i] = rcThermal[lum565(src[i])];
      break;
    case 6: // pencil sketch: inverted edges on paper
      for (int i = 0; i < 240 * 240; i++) rcLum[i] = lum565(src[i]);
      for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++) {
          int v = 255;
          if (x > 0 && y > 0 && x < 239 && y < 239) {
            const uint8_t *p = rcLum + y * 240 + x;
            int gx = -p[-241] - 2 * p[-1] - p[239] + p[-239] + 2 * p[1] + p[241];
            int gy = -p[-241] - 2 * p[-240] - p[-239] + p[239] + 2 * p[240] + p[241];
            v = 255 - min(255, (abs(gx) + abs(gy)) * 3 / 4);
          }
          dst[y * 240 + x] = RGB565(v, v, v * 15 / 16);
        }
      break;
    default:
      memcpy(dst, src, 240 * 240 * 2);
      break;
  }
  *ncolors = 256;
  return NULL;
}
void retroShareScreen() {
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print("Share");
  if (WiFi.status() != WL_CONNECTED) {
    drawCentered("Connect to WiFi to share", 110, 1, CLAUDE_GRAY);
  } else {
    String name = rcLastFile.substring(rcLastFile.lastIndexOf('/') + 1);
    String url = "http://" + WiFi.localIP().toString() + "/rc?f=" + name;
    drawQrCode(url, 120, 108, 150);
    drawCentered(name, 190, 1, CLAUDE_CREAM);
    drawCentered("Scan with a phone on this WiFi", 204, 1, CLAUDE_GRAY);
  }
  drawCentered("Any button: back   /gallery: all", 226, 1, CLAUDE_GRAY);
  pushUi();
}
void retroMode() {
  if (!retroInit()) {
    ui->fillScreen(CLAUDE_DARK);
    drawCentered("Not enough memory for Retro Cam", 110, 1, CLAUDE_RED);
    pushUi();
    return;
  }
  if (rcShare) { // the share QR stays until any button
    if (!rcShareDrawn) {
      retroShareScreen();
      rcShareDrawn = true;
    }
    if (pycamera.justPressed(AWEXP_BUTTON_OK) || pycamera.justPressed(SHUTTER_BUTTON) || pycamera.justPressed(AWEXP_BUTTON_UP) ||
        pycamera.justPressed(AWEXP_BUTTON_DOWN) || pycamera.justPressed(AWEXP_BUTTON_LEFT) || pycamera.justPressed(AWEXP_BUTTON_RIGHT))
      rcShare = false;
    return;
  }
  if (!rcGifFrames) { // the palette has to stay the same for a whole GIF
    if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) rcFilter = (rcFilter + RC_FILTERS - 1) % RC_FILTERS;
    if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) rcFilter = (rcFilter + 1) % RC_FILTERS;
  }
  if (!pycamera.captureFrame()) return;
  uint16_t *src = (uint16_t *)pycamera.fb->getBuffer(), *dst = ui->getBuffer();
  int ncolors;
  const uint16_t *pal = retroFilter(src, dst, &ncolors);
  esp_camera_fb_return(pycamera.frame); // we drew our own copy; hand the camera its buffer back
  pycamera.frame = NULL;
  if (pycamera.justPressed(SHUTTER_BUTTON) && !rcGifFrames) { // photo
    String path = sdNextName("/retrocam", "RC", "jpg");
    bool ok = path.length() && saveScreenJpeg(dst, path);
    rcToast = ok ? "Saved " + path.substring(10) : String(pycamera.SDdetected() ? "Couldn't save" : "No SD card");
    rcToastUntil = millis() + 2000;
    if (ok) {
      rcLastFile = path;
      rcFlashUntil = millis() + 120;
      pycamera.speaker_tone(1800, 30);
      petBoost(2);
    }
  }
  if (pycamera.justPressed(AWEXP_BUTTON_OK) && !rcGifFrames) { // 12-frame GIF
    rcGifPath = sdNextName("/retrocam", "RC", "gif");
    if (rcGifPath.length() && gifBegin(rcGifPath, pal ? pal : rcPal332, pal ? ncolors : 256)) {
      rcGifFrames = 1;
      rcGifNext = 0;
      pycamera.speaker_tone(1320, 30);
    } else {
      rcToast = pycamera.SDdetected() ? "Couldn't start the GIF" : "No SD card";
      rcToastUntil = millis() + 2000;
    }
  }
  if (rcGifFrames && millis() >= rcGifNext) {
    if (!pal) // full colour: map every pixel to the fixed 256-colour palette
      for (int i = 0; i < 240 * 240; i++) {
        uint16_t c = dst[i];
        rcIdx[i] = (((c >> 13) & 7) << 5) | (((c >> 8) & 7) << 2) | ((c >> 3) & 3);
      }
    gifFrame(rcIdx, 15);
    rcGifNext = millis() + 150;
    if (++rcGifFrames > 12) {
      gifEnd();
      rcGifFrames = 0;
      rcLastFile = rcGifPath;
      rcToast = "GIF saved " + rcGifPath.substring(10);
      rcToastUntil = millis() + 2500;
      pycamera.speaker_tone(1568, 60);
      petBoost(3);
    }
  }
  if (pycamera.justPressed(AWEXP_BUTTON_UP) && rcLastFile.length()) {
    rcShare = true;
    rcShareDrawn = false;
    return;
  }
  // overlay (drawn after saving, so photos and GIFs are clean)
  if (millis() < rcFlashUntil) ui->fillScreen(0xFFFF);
  ui->fillRect(0, 0, 240, 16, CLAUDE_DARK);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_ORANGE);
  String name = String("< ") + RC_NAMES[rcFilter] + " >";
  ui->setCursor(120 - name.length() * 3, 4);
  ui->print(name);
  if (rcGifFrames) {
    if ((millis() / 300) % 2) ui->fillCircle(8, 8, 4, CLAUDE_RED);
    ui->setTextColor(CLAUDE_RED);
    ui->setCursor(196, 4);
    ui->printf("GIF %d", rcGifFrames - 1);
  }
  ui->fillRect(0, 226, 240, 14, CLAUDE_DARK);
  if (millis() < rcToastUntil) drawCentered(rcToast, 229, 1, CLAUDE_ORANGE);
  else drawCentered(rcLastFile.length() ? "Shutter photo  OK gif  UP share" : "Shutter photo   OK gif", 229, 1, CLAUDE_CREAM);
  pushUi();
}

// ---- Now Playing: what's on your phone, with album art and controls ----
String npTitle, npArtist, npApp, npError;
bool npPlaying = false, npHasArt = false, npLoaded = false, npArtValid = false;
long npPos = 0, npDur = 0;
int32_t npTrack = 0, npArtTrack = 0;
unsigned long npAt = 0, npPollAt = 0;
uint16_t *npArt = NULL; // 120x120 album art
bool artToBuffer(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      if (x + i < 120 && y + j < 120) npArt[(y + j) * 120 + x + i] = bitmap[j * w + i];
  return true;
}
void handleNowPlayingResult(bool parsed, JsonDocument &doc) {
  if (!parsed) {
    npError = phoneError(netStatus);
    return;
  }
  if (!(doc["ok"] | false)) {
    npError = (const char *)(doc["error"] | "The phone couldn't read what's playing");
    return;
  }
  npError = "";
  npLoaded = true;
  npTitle = (const char *)(doc["title"] | "");
  npArtist = (const char *)(doc["artist"] | "");
  npApp = (const char *)(doc["app"] | "");
  npPlaying = doc["playing"] | false;
  npPos = doc["position_ms"] | 0L;
  npDur = doc["duration_ms"] | 0L;
  npHasArt = doc["has_art"] | false;
  npTrack = doc["track"] | 0;
  npAt = millis();
  if (!npHasArt) npArtValid = false;
}
void handleArtResult() {
  npArtTrack = npTrack; // don't retry a track whose art failed
  if (netStatus == 200 && artJpg && artJpgLen) {
    if (!npArt) npArt = (uint16_t *)ps_malloc(120 * 120 * 2);
    if (npArt) {
      TJpgDec.setJpgScale(1);
      TJpgDec.setCallback(artToBuffer); // captureFrame() sets its own callback back each time
      npArtValid = TJpgDec.drawJpg(0, 0, artJpg, artJpgLen) == JDR_OK;
    }
  }
  free(artJpg);
  artJpg = NULL;
  artJpgLen = 0;
}
void mediaCommand(const char *action) {
  if (!waitNetIdle(2000)) return;
  uint8_t *body = (uint8_t *)ps_malloc(3);
  if (!body) return;
  memcpy(body, "{}", 3);
  if (!startNetJob(JOB_MEDIA, String("/media?action=") + action, body, 2, "application/json")) free(body);
  npPollAt = millis() - 1200; // look again soon
}
String mmss(long ms) {
  char b[12];
  snprintf(b, sizeof(b), "%ld:%02ld", ms / 60000, ms / 1000 % 60);
  return b;
}
void drawMusicNote(int x, int y, int s, uint16_t c) { // a big quaver
  ui->fillCircle(x, y, 5 * s, c);
  ui->fillRect(x + 4 * s, y - 22 * s, 2 * s, 22 * s, c);
  ui->fillTriangle(x + 6 * s, y - 22 * s, x + 16 * s, y - 14 * s, x + 6 * s, y - 14 * s, c);
}
void nowPlayingMode() {
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
    npPlaying = !npPlaying;
    npPos += npPlaying ? 0 : (long)(millis() - npAt);
    npAt = millis();
    mediaCommand("play_pause");
  }
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) mediaCommand("previous");
  if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) mediaCommand("next");
  if (pycamera.justPressed(AWEXP_BUTTON_UP)) mediaCommand("volume_up");
  if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) mediaCommand("volume_down");
  if (!netBusy() && millis() - npPollAt > 2000) {
    npPollAt = millis();
    if (npHasArt && npTrack != npArtTrack) startNetJob(JOB_ART, "/nowplaying/art", NULL, 0, "");
    else startNetJob(JOB_NOWPLAY, "/nowplaying", NULL, 0, "");
  }
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 50) return;
  lastDraw = millis();
  unsigned long now = millis();
  ui->fillScreen(CLAUDE_DARK);
  if (npError.length() || !npLoaded) {
    drawMusicNote(108, 100, 2, CLAUDE_TRACK);
    String lines[3];
    int n = wrapText(npError.length() ? npError : String("Asking the phone what's playing..."), 34, lines, 3);
    for (int i = 0; i < n; i++) drawCentered(lines[i], 140 + i * 12, 1, npError.length() ? CLAUDE_CREAM : CLAUDE_GRAY);
    drawCentered("Needs the Clawd Phone app", 220, 1, CLAUDE_GRAY);
    pushUi();
    return;
  }
  if (!npTitle.length()) {
    drawMusicNote(108, 100, 2, CLAUDE_TRACK);
    drawCentered("Nothing playing on your phone", 140, 1, CLAUDE_CREAM);
    drawCentered("OK: play   < >: previous / next", 220, 1, CLAUDE_GRAY);
    pushUi();
    return;
  }
  // album art (or a big note), gently bobbing while it plays
  int bob = npPlaying ? (int)(2 * sinf(now / 300.0f)) : 0;
  if (npArtValid && npArt) {
    ui->drawRGBBitmap(60, 14 + bob, npArt, 120, 120);
    ui->drawRoundRect(58, 12 + bob, 124, 124, 6, CLAUDE_TRACK);
  } else {
    ui->fillRoundRect(60, 14 + bob, 120, 120, 10, 0x2104);
    drawMusicNote(112, 92 + bob, 2, CLAUDE_ORANGE);
  }
  // title (scrolls when long), artist, app
  String t = npTitle;
  if (t.length() > 19) {
    String loop = t + "     " + t;
    int off = (now / 180) % (t.length() + 5);
    t = loop.substring(off, off + 19);
  }
  drawCentered(t, 142, 2, CLAUDE_CREAM);
  drawCentered(npArtist.length() > 38 ? npArtist.substring(0, 37) + "." : npArtist, 162, 1, CLAUDE_ORANGE);
  drawCentered(npApp, 174, 1, CLAUDE_GRAY);
  // progress
  long pos = npPos + (npPlaying ? (long)(now - npAt) : 0);
  if (npDur > 0) pos = min(pos, npDur);
  ui->fillRoundRect(40, 190, 160, 5, 2, CLAUDE_TRACK);
  if (npDur > 0) ui->fillRoundRect(40, 190, max(5, (int)(160 * pos / npDur)), 5, 2, CLAUDE_ORANGE);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(4, 189);
  ui->print(mmss(pos));
  if (npDur > 0) {
    String d = mmss(npDur);
    ui->setCursor(236 - d.length() * 6, 189);
    ui->print(d);
  }
  // controls: previous, play/pause, next
  ui->fillTriangle(84, 214, 94, 207, 94, 221, CLAUDE_CREAM);
  ui->fillTriangle(76, 214, 86, 207, 86, 221, CLAUDE_CREAM);
  ui->fillCircle(120, 214, 12, CLAUDE_ORANGE);
  if (npPlaying) {
    ui->fillRect(114, 207, 4, 14, CLAUDE_DARK);
    ui->fillRect(122, 207, 4, 14, CLAUDE_DARK);
  } else {
    ui->fillTriangle(116, 206, 116, 222, 128, 214, CLAUDE_DARK);
  }
  ui->fillTriangle(156, 214, 146, 207, 146, 221, CLAUDE_CREAM);
  ui->fillTriangle(164, 214, 154, 207, 154, 221, CLAUDE_CREAM);
  // Clawd vibes along
  int arm = npPlaying ? ((now / 300) % 2 ? 2 : 1) : 0;
  drawBot(ui, 12, 216 - (npPlaying ? (int)(3 * fabsf(sinf(now / 300.0f))) : 0), 3, npPlaying ? FACE_HAPPY : FACE_BLINK, arm, 2 - arm);
  ui->setCursor(192, 226);
  ui->print("^v vol");
  pushUi();
}

// ---- Browser: the web as text, Lynx-style (the phone fetches and simplifies pages) ----
#define BR_MAX_LINES 400
#define BR_MAX_LINKS 40
#define BR_VISIBLE 17
const char *BR_HOME[] = {"Say a site or a search...", "en.wikipedia.org/wiki/Special:Random", "news.ycombinator.com",
                         "lite.cnn.com",                "text.npr.org",                          "github.com/trending"};
#define BR_HOME_COUNT 6
String brLines[BR_MAX_LINES];
uint8_t brKind[BR_MAX_LINES]; // 0 text, 1 heading
String brLinkText[BR_MAX_LINKS], brLinkUrl[BR_MAX_LINKS];
String brTitle, brUrl, brError, brPending, brLoadingWhat;
String brHistory[8];
int brLineCount = 0, brLinkCount = 0, brHistLen = 0;
int brState = 0; // 0 home, 1 loading, 2 page, 3 links
int brScroll = 0, brSel = 0, brHomeSel = 0;
bool brPushHistory = true;
#define LP_BROWSER 1
#define LP_HUNT 2
int listenPurpose = 0;
void browserOpen(const String &input, bool remember) {
  brPending = input;
  brPushHistory = remember;
  brLoadingWhat = input;
  brState = 1;
  brError = "";
}
// Called from browserMode(): sends a queued page request once the network is free
void browserTick() {
  if (!brPending.length() || netBusy()) return;
  JsonDocument req;
  req["input"] = brPending;
  size_t len = measureJson(req);
  uint8_t *body = (uint8_t *)ps_malloc(len + 1);
  if (body) serializeJson(req, (char *)body, len + 1);
  if (body && startNetJob(JOB_BROWSE, "/browse", body, len, "application/json")) {
    brPending = "";
  } else {
    free(body);
  }
}
void handleBrowseResult(bool parsed, JsonDocument &doc) {
  if (!parsed || !(doc["ok"] | false)) {
    brError = parsed ? String((const char *)(doc["error"] | "Couldn't open that")) : phoneError(netStatus);
    brState = brLineCount ? 2 : 0;
    return;
  }
  if (brPushHistory && brUrl.length()) {
    if (brHistLen == 8) {
      for (int i = 1; i < 8; i++) brHistory[i - 1] = brHistory[i];
      brHistLen--;
    }
    brHistory[brHistLen++] = brUrl;
  }
  brTitle = (const char *)(doc["title"] | "");
  brUrl = (const char *)(doc["url"] | "");
  brLineCount = 0;
  String wrapped[24];
  for (JsonObject b : doc["blocks"].as<JsonArray>()) {
    bool heading = strcmp(b["t"] | "p", "h") == 0;
    int n = wrapText((const char *)(b["x"] | ""), 38, wrapped, 24);
    for (int i = 0; i < n && brLineCount < BR_MAX_LINES; i++) {
      brKind[brLineCount] = heading;
      brLines[brLineCount++] = wrapped[i];
    }
    if (brLineCount < BR_MAX_LINES) {
      brKind[brLineCount] = 0;
      brLines[brLineCount++] = "";
    }
  }
  brLinkCount = 0;
  for (JsonObject l : doc["links"].as<JsonArray>()) {
    if (brLinkCount >= BR_MAX_LINKS) break;
    brLinkText[brLinkCount] = (const char *)(l["text"] | "");
    brLinkUrl[brLinkCount++] = (const char *)(l["url"] | "");
  }
  brScroll = 0;
  brSel = 0;
  brState = 2;
  pycamera.speaker_tone(1320, 15);
}
void browserListen() {
  if (!waitNetIdle(4000)) return;
  listenPurpose = LP_BROWSER;
  if (startNetJob(JOB_LISTEN2, "/listen", NULL, 0, "")) {
    brState = 1;
    brLoadingWhat = "Listening... speak to your phone";
  }
}
void drawBrowserTitle(const String &title) {
  ui->fillRect(0, 0, 240, 15, 0x2104);
  drawClaudeLogo(ui, 8, 7, 5, 0, CLAUDE_ORANGE);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(17, 4);
  ui->print(title.length() > 36 ? title.substring(0, 35) + "." : title);
}
void browserMode() {
  browserTick();
  unsigned long now = millis();
  if (pycamera.justPressed(SHUTTER_BUTTON)) browserListen();
  if (brState == 0) { // home: bookmarks
    if (pycamera.justPressed(AWEXP_BUTTON_UP)) brHomeSel = (brHomeSel + BR_HOME_COUNT - 1) % BR_HOME_COUNT;
    if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) brHomeSel = (brHomeSel + 1) % BR_HOME_COUNT;
    if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
      if (brHomeSel == 0) browserListen();
      else browserOpen(BR_HOME[brHomeSel], true);
    }
    if (pycamera.justPressed(AWEXP_BUTTON_RIGHT) && brLineCount) brState = 2;
  } else if (brState == 2) { // reading
    if (pressedOrRepeat(AWEXP_BUTTON_DOWN)) brScroll = min(max(0, brLineCount - BR_VISIBLE), brScroll + 3);
    if (pressedOrRepeat(AWEXP_BUTTON_UP)) brScroll = max(0, brScroll - 3);
    if (pycamera.justPressed(AWEXP_BUTTON_OK) && brLinkCount) brState = 3;
    if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) {
      if (brHistLen) browserOpen(brHistory[--brHistLen], false);
      else brState = 0;
    }
  } else if (brState == 3) { // picking a link
    if (pressedOrRepeat(AWEXP_BUTTON_DOWN)) brSel = min(brLinkCount - 1, brSel + 1);
    if (pressedOrRepeat(AWEXP_BUTTON_UP)) brSel = max(0, brSel - 1);
    if (pycamera.justPressed(AWEXP_BUTTON_OK)) browserOpen(brLinkUrl[brSel], true);
    if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) brState = 2;
  }
  static unsigned long lastDraw = 0;
  if (now - lastDraw < 50) return;
  lastDraw = now;
  ui->fillScreen(CLAUDE_DARK);
  if (brState == 0) {
    drawBrowserTitle("Clawd Browser");
    for (int i = 0; i < BR_HOME_COUNT; i++) {
      int y = 24 + i * 30;
      bool sel = i == brHomeSel;
      if (sel) ui->fillRoundRect(6, y, 228, 26, 8, CLAUDE_ORANGE);
      else ui->drawRoundRect(6, y, 228, 26, 8, CLAUDE_TRACK);
      ui->setTextSize(1);
      ui->setTextColor(sel ? CLAUDE_DARK : CLAUDE_CREAM);
      ui->setCursor(16, y + 9);
      String s = BR_HOME[i];
      ui->print(s.length() > 35 ? s.substring(0, 34) + "." : s);
    }
    if (brError.length()) drawCentered(brError.substring(0, 38), 208, 1, CLAUDE_RED);
    drawCentered(brLineCount ? "OK open  Shutter speak  > back to page" : "OK open   Shutter: speak a site", 226, 1, CLAUDE_GRAY);
  } else if (brState == 1) {
    drawBrowserTitle("Loading");
    drawClaudeLogo(ui, 120, 100, 26, now * 0.3f, CLAUDE_ORANGE);
    String lines[3];
    int n = wrapText(brLoadingWhat, 34, lines, 3);
    for (int i = 0; i < n; i++) drawCentered(lines[i], 148 + i * 12, 1, CLAUDE_CREAM);
    drawCentered("The phone fetches it, the board reads it", 226, 1, CLAUDE_GRAY);
  } else if (brState == 2) {
    drawBrowserTitle(brTitle.length() ? brTitle : brUrl);
    ui->setTextSize(1);
    for (int i = 0; i < BR_VISIBLE && brScroll + i < brLineCount; i++) {
      ui->setTextColor(brKind[brScroll + i] ? CLAUDE_ORANGE : CLAUDE_CREAM);
      ui->setCursor(4, 20 + i * 11);
      ui->print(brLines[brScroll + i]);
    }
    if (brLineCount > BR_VISIBLE) { // scrollbar
      int h = max(10, 186 * BR_VISIBLE / brLineCount), y = 20 + (186 - h) * brScroll / max(1, brLineCount - BR_VISIBLE);
      ui->fillRect(236, y, 3, h, CLAUDE_TRACK);
    }
    if (brError.length()) drawCentered(brError.substring(0, 38), 212, 1, CLAUDE_RED);
    drawCentered(String("^v scroll  OK links(") + brLinkCount + ")  < back", 228, 1, CLAUDE_GRAY);
  } else {
    drawBrowserTitle("Links on this page");
    int first = constrain(brSel - 6, 0, max(0, brLinkCount - 14));
    for (int i = first; i < first + 14 && i < brLinkCount; i++) {
      int y = 20 + (i - first) * 14;
      if (i == brSel) ui->fillRect(0, y - 2, 240, 13, CLAUDE_ORANGE);
      ui->setTextColor(i == brSel ? CLAUDE_DARK : CLAUDE_CREAM);
      ui->setCursor(4, y);
      ui->print(brLinkText[i].length() > 38 ? brLinkText[i].substring(0, 37) + "." : brLinkText[i]);
    }
    drawCentered("^v pick   OK open   < back to page", 228, 1, CLAUDE_GRAY);
  }
  pushUi();
}

// ---- Friends: Clawds that meet over the air (ESP-NOW, no router or phone needed) ----
// Every board beacons its name and pet stats every few seconds. Boards on the same WiFi
// channel (e.g. the same router) see each other, swap profiles, chat and play Pong Duel.
#define NOW_BEACON 1
#define NOW_CHAT 2
#define NOW_WAVE 3
#define NOW_PONG_STATE 4
#define NOW_PONG_PADDLE 5
struct NowMsg {
  uint8_t mac[6];
  int8_t rssi;
  ClawdPacket p;
};
struct Friend {
  uint8_t mac[6];
  char name[13];
  uint8_t food, energy, fun;
  uint16_t meets;
};
#define MAX_FRIENDS 20
Friend friends[MAX_FRIENDS];
int friendCount = 0;
unsigned long friendSeenAt[MAX_FRIENDS]; // 0 = not seen since boot
int8_t friendRssi[MAX_FRIENDS];
bool friendLobby[MAX_FRIENDS];
QueueHandle_t nowQueue = NULL;
bool nowReady = false;
uint8_t myMac[6];
String myName;
unsigned long nowBeaconAt = 0;
String newFriendName;
unsigned long newFriendUntil = 0;
// chat history (most recent last)
#define CHAT_KEEP 8
String chatText[CHAT_KEEP];
int8_t chatWho[CHAT_KEEP]; // friend index, or -1 for me
int chatCount = 0;
const char *CHAT_CANNED[] = {"Hi!", "Wanna play Pong Duel?", "On my way", "Nice!", "GG", "Brb", "Look up!", "<3", "Feed your Clawd!"};
#define CHAT_CANNED_COUNT 9
int friendsSel = 0, chatPick = 0, chatWith = -1;
unsigned long waveFromUntil = 0;
bool duelLobby = false; // set by Pong Duel; shared in our beacon
void onNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(ClawdPacket) || memcmp(data, "CLWD", 4) != 0 || !nowQueue) return;
  NowMsg m;
  memcpy(m.mac, info->src_addr, 6);
  m.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
  memcpy(&m.p, data, sizeof(ClawdPacket));
  xQueueSend(nowQueue, &m, 0); // handled in loop(), off the WiFi task
}
void saveFriends() {
  prefs.putBytes("friends", friends, sizeof(Friend) * friendCount);
}
void nowInit() {
  if (nowReady || WiFi.getMode() == WIFI_OFF) return;
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  myName = CLAWD_NAME;
  if (!myName.length()) {
    char n[13];
    snprintf(n, sizeof(n), "Clawd-%02X%02X", myMac[4], myMac[5]);
    myName = n;
  }
  if (esp_now_init() != ESP_OK) return;
  nowQueue = xQueueCreate(12, sizeof(NowMsg));
  esp_now_register_recv_cb(onNowRecv);
  esp_now_peer_info_t peer = {};
  memset(peer.peer_addr, 0xFF, 6); // broadcast
  peer.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&peer);
  size_t bytes = prefs.getBytesLength("friends");
  if (bytes && bytes % sizeof(Friend) == 0 && bytes <= sizeof(friends)) {
    prefs.getBytes("friends", friends, bytes);
    friendCount = bytes / sizeof(Friend);
  }
  nowReady = true;
}
void nowSend(const uint8_t *mac, uint8_t type, const char *text, int16_t a, int16_t b, int16_t c, int16_t d, int16_t e, int16_t f) {
  if (!nowReady) return;
  ClawdPacket p = {};
  memcpy(p.magic, "CLWD", 4);
  p.type = type;
  strlcpy(p.name, myName.c_str(), sizeof(p.name));
  p.food = pet.food;
  p.energy = pet.energy;
  p.fun = pet.fun;
  p.flags = duelLobby ? 1 : 0;
  p.a = a;
  p.b = b;
  p.c = c;
  p.d = d;
  p.e = e;
  p.f = f;
  if (text) strlcpy(p.text, text, sizeof(p.text));
  if (mac[0] != 0xFF && !esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
  }
  esp_now_send(mac, (uint8_t *)&p, sizeof(p));
}
const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
int findFriend(const uint8_t *mac) {
  for (int i = 0; i < friendCount; i++)
    if (!memcmp(friends[i].mac, mac, 6)) return i;
  return -1;
}
bool friendHere(int i) {
  return i >= 0 && friendSeenAt[i] && millis() - friendSeenAt[i] < 15000;
}
void addChat(int who, const String &text) {
  if (chatCount == CHAT_KEEP) {
    for (int i = 1; i < CHAT_KEEP; i++) {
      chatText[i - 1] = chatText[i];
      chatWho[i - 1] = chatWho[i];
    }
    chatCount--;
  }
  chatText[chatCount] = text;
  chatWho[chatCount++] = who;
}
void duelOnPacket(int who, const ClawdPacket &p);
// Called every loop(): beacons, and handles what other Clawds sent
void friendsTick() {
  if (!nowReady) {
    if (WiFi.getMode() != WIFI_OFF && millis() > 3000) nowInit();
    return;
  }
  if (millis() - nowBeaconAt > (duelLobby ? 1000UL : 3000UL)) {
    nowBeaconAt = millis();
    nowSend(BROADCAST_MAC, NOW_BEACON, NULL, 0, 0, 0, 0, 0, 0);
  }
  NowMsg m;
  while (xQueueReceive(nowQueue, &m, 0) == pdTRUE) {
    int i = findFriend(m.mac);
    if (i < 0 && m.p.type == NOW_BEACON && friendCount < MAX_FRIENDS) { // a new friend!
      i = friendCount++;
      memcpy(friends[i].mac, m.mac, 6);
      friends[i].meets = 0;
      friendSeenAt[i] = 0;
      newFriendName = m.p.name;
      newFriendUntil = millis() + 4000;
      petBoost(10);
      if (!danceOn && !musicPlaying) pycamera.speaker_tone(1568, 40);
    }
    if (i < 0) continue;
    if (!friendSeenAt[i] || millis() - friendSeenAt[i] > 10UL * 60 * 1000) { // a new meeting
      friends[i].meets++;
      strlcpy(friends[i].name, m.p.name, sizeof(friends[i].name));
      friends[i].food = m.p.food;
      friends[i].energy = m.p.energy;
      friends[i].fun = m.p.fun;
      saveFriends();
    }
    friendSeenAt[i] = millis();
    friendRssi[i] = m.rssi;
    friendLobby[i] = m.p.flags & 1;
    friends[i].food = m.p.food;
    friends[i].energy = m.p.energy;
    friends[i].fun = m.p.fun;
    if (m.p.type == NOW_CHAT) {
      m.p.text[sizeof(m.p.text) - 1] = 0;
      addChat(i, m.p.text);
      if (!danceOn && !musicPlaying) pycamera.speaker_tone(1320, 30);
    } else if (m.p.type == NOW_WAVE) {
      addChat(i, String("*waves*"));
      waveFromUntil = millis() + 2500;
      if (!danceOn && !musicPlaying) pycamera.speaker_tone(1760, 30);
    } else if (m.p.type == NOW_PONG_STATE || m.p.type == NOW_PONG_PADDLE) {
      duelOnPacket(i, m.p);
    }
  }
}
uint16_t friendColor(int i) { // a stable colour per friend, from its address
  static const uint16_t C[6] = {CLAUDE_ORANGE, CLAUDE_BLUE, CLAUDE_GREEN, CLAUDE_AMBER, 0xA37A, CLAUDE_RED};
  return C[(friends[i].mac[4] + friends[i].mac[5]) % 6];
}
void friendsMode() {
  unsigned long now = millis();
  if (chatWith < 0) { // the list
    if (pycamera.justPressed(AWEXP_BUTTON_UP) && friendCount) friendsSel = (friendsSel + friendCount - 1) % friendCount;
    if (pycamera.justPressed(AWEXP_BUTTON_DOWN) && friendCount) friendsSel = (friendsSel + 1) % friendCount;
    if (pycamera.justPressed(AWEXP_BUTTON_OK) && friendCount) {
      chatWith = friendsSel;
      chatPick = 0;
    }
    if (pycamera.justPressed(AWEXP_BUTTON_RIGHT) && friendHere(friendsSel)) {
      nowSend(friends[friendsSel].mac, NOW_WAVE, NULL, 0, 0, 0, 0, 0, 0);
      addChat(-1, "*waves at " + String(friends[friendsSel].name) + "*");
    }
  } else { // chatting
    if (pycamera.justPressed(AWEXP_BUTTON_UP)) chatPick = (chatPick + CHAT_CANNED_COUNT - 1) % CHAT_CANNED_COUNT;
    if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) chatPick = (chatPick + 1) % CHAT_CANNED_COUNT;
    if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
      nowSend(friends[chatWith].mac, NOW_CHAT, CHAT_CANNED[chatPick], 0, 0, 0, 0, 0, 0);
      addChat(-1, CHAT_CANNED[chatPick]);
      pycamera.speaker_tone(988, 15);
    }
    if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) {
      nowSend(friends[chatWith].mac, NOW_WAVE, NULL, 0, 0, 0, 0, 0, 0);
      addChat(-1, String("*waves*"));
    }
    if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) chatWith = -1;
  }
  static unsigned long lastDraw = 0;
  if (now - lastDraw < 66) return;
  lastDraw = now;
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  if (chatWith < 0) {
    ui->print("Friends");
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(232 - myName.length() * 6, 15);
    ui->print(myName);
    if (!nowReady) {
      drawCentered("Starting the radio...", 110, 1, CLAUDE_GRAY);
    } else if (!friendCount) {
      drawBot(ui, 85, 70, 10, (now / 700) % 2 ? FACE_LOOK_LEFT : FACE_LOOK_RIGHT, 0, 0);
      drawCentered("Looking for other Clawds...", 140, 1, CLAUDE_CREAM);
      drawCentered("Another MEMENTO running Clawd Handy", 160, 1, CLAUDE_GRAY);
      drawCentered("on the same WiFi shows up here.", 172, 1, CLAUDE_GRAY);
    } else {
      int first = constrain(friendsSel - 2, 0, max(0, friendCount - 5));
      for (int i = first; i < first + 5 && i < friendCount; i++) {
        int y = 36 + (i - first) * 36;
        bool sel = i == friendsSel;
        ui->drawRoundRect(6, y, 228, 32, 8, sel ? CLAUDE_ORANGE : CLAUDE_TRACK);
        drawBot(ui, 16, y + 10, 3, friendHere(i) ? FACE_HAPPY : FACE_BLINK, friendHere(i) && (now / 300) % 2 ? 2 : 0, 0);
        ui->fillCircle(46, y + 16, 3, friendColor(i));
        ui->setTextSize(1);
        ui->setTextColor(CLAUDE_CREAM);
        ui->setCursor(54, y + 6);
        ui->print(friends[i].name);
        ui->setTextColor(CLAUDE_GRAY);
        ui->setCursor(54, y + 19);
        ui->printf("met %d time%s  fed %d%%  fun %d%%", friends[i].meets, friends[i].meets == 1 ? "" : "s", friends[i].food, friends[i].fun);
        if (friendHere(i)) { // signal bars
          int bars = friendRssi[i] > -55 ? 3 : friendRssi[i] > -70 ? 2 : 1;
          for (int b = 0; b < 3; b++) ui->fillRect(206 + b * 6, y + 18 - b * 4, 4, 6 + b * 4, b < bars ? CLAUDE_GREEN : CLAUDE_TRACK);
        } else {
          ui->setCursor(196, y + 6);
          ui->print("away");
        }
      }
    }
    if (now < newFriendUntil) {
      ui->fillRoundRect(20, 196, 200, 20, 8, CLAUDE_ORANGE);
      drawCentered("New friend: " + newFriendName + "!", 202, 1, CLAUDE_DARK);
    }
    drawCentered(friendCount ? "OK chat   > wave   (pet +10 fun per friend)" : "Friends make your pet happy", 226, 1, CLAUDE_GRAY);
  } else {
    ui->print(friends[chatWith].name);
    ui->setTextSize(1);
    int y = 36;
    for (int i = max(0, chatCount - 6); i < chatCount; i++) {
      bool mine = chatWho[i] < 0;
      if (!mine && chatWho[i] != chatWith) continue;
      int w = min(200, (int)chatText[i].length() * 6 + 16);
      int x = mine ? 234 - w : 6;
      ui->fillRoundRect(x, y, w, 18, 8, mine ? CLAUDE_ORANGE : CLAUDE_CREAM);
      ui->setTextColor(CLAUDE_DARK);
      ui->setCursor(x + 8, y + 5);
      ui->print(chatText[i].substring(0, 31));
      y += 22;
    }
    if (now < waveFromUntil) drawBot(ui, 180, 40, 4, FACE_HAPPY, (now / 150) % 2 ? 2 : 1, 0);
    ui->fillRoundRect(6, 172, 228, 24, 8, 0x2104);
    ui->setTextColor(CLAUDE_ORANGE);
    ui->setCursor(14, 180);
    ui->printf("^ %s v", CHAT_CANNED[chatPick]);
    drawCentered(friendHere(chatWith) ? "OK send  > wave  < back" : "Away right now - they'll get it when near", 226, 1, CLAUDE_GRAY);
  }
  pushUi();
}

// ---- Pong Duel: 2 players, one Clawd each, over ESP-NOW ----
int duelState = 0; // 0 lobby, 1 playing
int duelPeer = -1;
bool duelHost = false;
float dBallX, dBallY, dVX, dVY;
int dHostY = 90, dGuestY = 90, dScoreH = 0, dScoreG = 0;
unsigned long duelRxAt = 0;
void duelReset() {
  dBallX = 120;
  dBallY = 120;
  dVX = (random(2) ? 2.6f : -2.6f);
  dVY = random(-20, 21) / 10.0f;
}
void duelStart() {
  duelLobby = true;
  duelState = 0;
  duelPeer = -1;
  dScoreH = dScoreG = 0;
  nowBeaconAt = 0; // tell everyone right away
}
void duelOnPacket(int who, const ClawdPacket &p) {
  if (currentMode != MODE_GAME || currentGame != GAME_DUEL || who != duelPeer) return;
  duelRxAt = millis();
  if (p.type == NOW_PONG_STATE && !duelHost) {
    dBallX = p.a / 10.0f;
    dBallY = p.b / 10.0f;
    dHostY = p.c;
    dScoreH = p.d;
    dScoreG = p.e;
    if (p.f & 1) pycamera.speaker_tone(660, 20);
  } else if (p.type == NOW_PONG_PADDLE && duelHost) {
    dGuestY = p.a;
  }
}
void duelUpdate() {
  if (duelState == 0) { // lobby: pair with the first friend also waiting
    for (int i = 0; i < friendCount; i++)
      if (friendHere(i) && friendLobby[i] && millis() - friendSeenAt[i] < 4000) {
        duelPeer = i;
        duelHost = memcmp(myMac, friends[i].mac, 6) < 0; // the lower address runs the ball
        duelState = 1;
        duelRxAt = millis();
        dHostY = dGuestY = 90;
        duelReset();
        pycamera.speaker_tone(1046, 60);
        break;
      }
    return;
  }
  int &mine = duelHost ? dHostY : dGuestY;
  if (isHeld(AWEXP_BUTTON_UP)) mine = max(20, mine - 6);
  if (isHeld(AWEXP_BUTTON_DOWN)) mine = min(196, mine + 6);
  if (millis() - duelRxAt > 4000) { // they left
    duelState = 0;
    return;
  }
  if (duelHost) {
    uint8_t sound = 0;
    dBallX += dVX;
    dBallY += dVY;
    if (dBallY < 24 || dBallY > 232) {
      dVY = -dVY;
      dBallY = constrain(dBallY, 24.0f, 232.0f);
    }
    if (dBallX < 14 && dBallY > dHostY - 4 && dBallY < dHostY + 44 && dVX < 0) { // host paddle, left
      dVX = -dVX * 1.06f;
      dVY += (dBallY - dHostY - 20) / 10.0f;
      sound = 1;
    }
    if (dBallX > 226 && dBallY > dGuestY - 4 && dBallY < dGuestY + 44 && dVX > 0) { // guest paddle, right
      dVX = -dVX * 1.06f;
      dVY += (dBallY - dGuestY - 20) / 10.0f;
      sound = 1;
    }
    if (dBallX < 0 || dBallX > 240) {
      if (dBallX < 0) dScoreG++;
      else dScoreH++;
      duelReset();
      sound = 1;
    }
    if (sound) pycamera.speaker_tone(660, 15);
    nowSend(friends[duelPeer].mac, NOW_PONG_STATE, NULL, (int16_t)(dBallX * 10), (int16_t)(dBallY * 10), dHostY, dScoreH, dScoreG, sound);
  } else {
    nowSend(friends[duelPeer].mac, NOW_PONG_PADDLE, NULL, dGuestY, 0, 0, 0, 0, 0);
  }
  int me = duelHost ? dScoreH : dScoreG, them = duelHost ? dScoreG : dScoreH;
  if (me >= 7 || them >= 7) {
    gameScore = me;
    duelLobby = false;
    endGame();
  }
}
void duelDraw() {
  ui->fillScreen(CLAUDE_DARK);
  if (duelState == 0) {
    drawBot(ui, 85, 70, 10, (millis() / 600) % 2 ? FACE_LOOK_LEFT : FACE_LOOK_RIGHT, 1, 1);
    drawCentered("Pong Duel", 20, 2, CLAUDE_ORANGE);
    drawCentered("Waiting for a friend...", 140, 1, CLAUDE_CREAM);
    drawCentered("Open Games > Pong Duel on another", 160, 1, CLAUDE_GRAY);
    drawCentered("MEMENTO on the same WiFi", 172, 1, CLAUDE_GRAY);
    return;
  }
  for (int y = 20; y < 240; y += 12) ui->fillRect(119, y, 2, 6, CLAUDE_TRACK);
  ui->fillRoundRect(4, dHostY, 6, 40, 3, duelHost ? CLAUDE_ORANGE : CLAUDE_CREAM);
  ui->fillRoundRect(230, dGuestY, 6, 40, 3, duelHost ? CLAUDE_CREAM : CLAUDE_ORANGE);
  ui->fillCircle(dBallX, dBallY, 4, CLAUDE_CREAM);
  ui->setTextSize(3);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(80, 4);
  ui->print(dScoreH);
  ui->setCursor(146, 4);
  ui->print(dScoreG);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_ORANGE);
  ui->setCursor(duelHost ? 6 : 150, 228);
  ui->print("you");
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(duelHost ? 150 : 6, 228);
  ui->print(String(friends[duelPeer].name).substring(0, 13));
}

// ---- Clawd Beats: a rhythm game on top of the dance music ----
int beatsSong = 0, beatsCombo = 0, beatsBest = 0, beatsLoops = 0, beatsPrevStep = -1;
long beatsHitStep[32]; // steps already hit (or missed), by step % 32
String beatsJudge;
unsigned long beatsJudgeAt = 0;
int beatsJudgeLane = 0;
const int BEATS_LEAD = 16, BEATS_LEN = 256; // one bar intro, then 16 bars of notes
// Which lane (0 left, 1 middle, 2 right) has a note on this step, or -1
int beatsLane(long step) {
  if (step < BEATS_LEAD || step >= BEATS_LEAD + BEATS_LEN) return -1;
  char d = SONGS[beatsSong].drums[step % 16];
  if (d == 'K') return 0;
  if (d == 'S') return 2;
  if (d == 'O' || (d == 'H' && step % 8 == 2)) return 1;
  return -1;
}
float beatsNow() { // song position in 16th notes, counting loops
  int step;
  float phase;
  danceClock(step, phase);
  if (beatsPrevStep >= 0 && step < beatsPrevStep) beatsLoops++;
  beatsPrevStep = step;
  return beatsLoops * 64 + step + phase;
}
void beatsStart() {
  beatsSong = random(SONG_COUNT);
  beatsCombo = beatsBest = beatsLoops = 0;
  beatsPrevStep = -1;
  for (int i = 0; i < 32; i++) beatsHitStep[i] = -1;
  beatsJudge = "";
  startDance(beatsSong);
}
void beatsJudgeHit(const char *text, int lane) {
  beatsJudge = text;
  beatsJudgeAt = millis();
  beatsJudgeLane = lane;
}
void beatsUpdate() {
  float now = beatsNow();
  int pressed = pycamera.justPressed(AWEXP_BUTTON_LEFT) ? 0 : (pycamera.justPressed(AWEXP_BUTTON_UP) || pycamera.justPressed(AWEXP_BUTTON_OK)) ? 1 : pycamera.justPressed(AWEXP_BUTTON_RIGHT) ? 2 : -1;
  if (pressed >= 0) {
    long best = -1;
    float bestD = 1;
    for (long s = (long)now - 1; s <= (long)now + 1; s++) {
      if (beatsLane(s) != pressed || beatsHitStep[s % 32] == s) continue;
      float d = fabsf(s - now);
      if (d < bestD) {
        bestD = d;
        best = s;
      }
    }
    if (best >= 0 && bestD <= 0.6f) {
      beatsHitStep[best % 32] = best;
      beatsCombo++;
      beatsBest = max(beatsBest, beatsCombo);
      bool perfect = bestD <= 0.22f;
      gameScore += (perfect ? 100 : 50) + min(beatsCombo, 50) * 2;
      beatsJudgeHit(perfect ? "PERFECT" : "GOOD", pressed);
    } else {
      beatsCombo = 0;
      beatsJudgeHit("MISS", pressed);
    }
  }
  // notes that went past the line unhit
  long past = (long)(now - 0.7f);
  if (beatsLane(past) >= 0 && beatsHitStep[past % 32] != past) {
    beatsHitStep[past % 32] = past;
    beatsCombo = 0;
    beatsJudgeHit("MISS", beatsLane(past));
  }
  if (now >= BEATS_LEAD + BEATS_LEN + 4) { // song over
    stopDance();
    endGame();
  }
}
void beatsDraw() {
  float now = beatsNow();
  const int LANE_X[3] = {50, 120, 190};
  const uint16_t LANE_C[3] = {CLAUDE_ORANGE, CLAUDE_CREAM, CLAUDE_AMBER};
  const int HIT_Y = 196;
  ui->fillScreen(CLAUDE_DARK);
  for (int l = 0; l < 3; l++) ui->drawFastVLine(LANE_X[l], 24, 200, 0x18C3);
  bool kick = SONGS[beatsSong].drums[((int)now) % 16] == 'K';
  ui->drawFastHLine(20, HIT_Y, 200, kick ? CLAUDE_ORANGE : CLAUDE_TRACK);
  for (long s = (long)now - 1; s < (long)now + 9; s++) { // falling notes
    int lane = beatsLane(s);
    if (lane < 0 || beatsHitStep[s % 32] == s) continue;
    int y = HIT_Y - (int)((s - now) * 22);
    if (y < 20 || y > 236) continue;
    ui->fillRoundRect(LANE_X[lane] - 18, y - 6, 36, 12, 5, LANE_C[lane]);
  }
  for (int l = 0; l < 3; l++) { // the three buttons
    bool held = isHeld(l == 0 ? AWEXP_BUTTON_LEFT : l == 1 ? AWEXP_BUTTON_UP : AWEXP_BUTTON_RIGHT) || (l == 1 && isHeld(AWEXP_BUTTON_OK));
    ui->drawCircle(LANE_X[l], HIT_Y + 18, 9, LANE_C[l]);
    if (held) ui->fillCircle(LANE_X[l], HIT_Y + 18, 7, LANE_C[l]);
    ui->setTextSize(1);
    ui->setTextColor(held ? CLAUDE_DARK : LANE_C[l]);
    ui->setCursor(LANE_X[l] - 2, HIT_Y + 15);
    ui->print(l == 0 ? "<" : l == 1 ? "^" : ">");
  }
  if (millis() - beatsJudgeAt < 500 && beatsJudge.length()) {
    uint16_t c = beatsJudge == "PERFECT" ? CLAUDE_GREEN : beatsJudge == "GOOD" ? CLAUDE_AMBER : CLAUDE_RED;
    ui->setTextSize(1);
    ui->setTextColor(c);
    ui->setCursor(LANE_X[beatsJudgeLane] - beatsJudge.length() * 3, HIT_Y - 22);
    ui->print(beatsJudge);
  }
  // score, combo, song, and a little Clawd dancing along
  ui->fillRect(0, 0, 240, 22, CLAUDE_DARK);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(6, 4);
  ui->print(gameScore);
  if (beatsCombo > 2) {
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_ORANGE);
    ui->setCursor(6, 22);
    ui->printf("%d combo", beatsCombo);
  }
  int step;
  float phase;
  danceClock(step, phase);
  int dx, dy, la, ra;
  Face face;
  dancePose(step, phase, (step / 16) % 4, dx, dy, la, ra, face);
  drawBot(ui, 196 + dx / 4, 8 + dy / 3, 4, face, ra, la);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  if (now < BEATS_LEAD) drawCentered(String(SONGS[beatsSong].name) + " - get ready!", 110, 1, CLAUDE_CREAM);
  int left = max(0, (int)((BEATS_LEAD + BEATS_LEN - now) * 15000.0f / danceBpm / 1000));
  ui->setCursor(150, 4);
  ui->printf("%d:%02d", left / 60, left % 60);
}

// ---- Person Detector: a neural network running on the board itself ----
const tflite::Model *aiModel = nullptr;
tflite::MicroInterpreter *aiInterp = nullptr;
uint8_t *aiArena = nullptr;
#define AI_ARENA_SIZE (160 * 1024)
float aiPerson = 0;
unsigned long aiMs = 0, aiSeenAt = 0;
int aiState = 0; // 0 not started, 1 ready, -1 failed
String aiError;
bool aiInit() {
  if (aiState) return aiState > 0;
  aiState = -1;
  aiModel = tflite::GetModel(person_detect_model);
  if (aiModel->version() != TFLITE_SCHEMA_VERSION) {
    aiError = "Model version mismatch";
    return false;
  }
  static tflite::MicroMutableOpResolver<5> resolver;
  resolver.AddAveragePool2D();
  resolver.AddConv2D();
  resolver.AddDepthwiseConv2D();
  resolver.AddReshape();
  resolver.AddSoftmax();
  aiArena = (uint8_t *)heap_caps_malloc(AI_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!aiArena) {
    aiError = "Not enough memory for the model";
    return false;
  }
  static tflite::MicroInterpreter interpreter(aiModel, resolver, aiArena, AI_ARENA_SIZE);
  aiInterp = &interpreter;
  if (aiInterp->AllocateTensors() != kTfLiteOk) {
    aiError = "The model didn't fit in memory";
    return false;
  }
  aiState = 1;
  return true;
}
void aiTool() {
  if (!aiInit()) {
    ui->fillScreen(CLAUDE_DARK);
    drawCentered("Person Detector", 60, 2, CLAUDE_ORANGE);
    drawCentered(aiError, 110, 1, CLAUDE_RED);
    pushUi();
    return;
  }
  if (!pycamera.captureFrame()) return;
  uint16_t *src = (uint16_t *)pycamera.fb->getBuffer();
  TfLiteTensor *in = aiInterp->input(0);
  for (int y = 0; y < 96; y++) // 240x240 colour -> 96x96 grayscale, as the model expects
    for (int x = 0; x < 96; x++) in->data.int8[y * 96 + x] = (int8_t)(lum565(src[(y * 5 / 2) * 240 + x * 5 / 2]) - 128);
  unsigned long t0 = millis();
  if (aiInterp->Invoke() == kTfLiteOk) {
    aiMs = millis() - t0;
    TfLiteTensor *out = aiInterp->output(0);
    float p = (out->data.int8[1] - out->params.zero_point) * out->params.scale;
    aiPerson = aiPerson * 0.5f + constrain(p, 0.0f, 1.0f) * 0.5f;
  }
  bool seen = aiPerson > 0.65f;
  if (seen && millis() - aiSeenAt > 5000) pycamera.speaker_tone(1320, 25); // someone arrived
  if (seen) aiSeenAt = millis();
  PyCameraFB *fb = pycamera.fb;
  drawCameraTitle(fb, "Person Detector - on the board");
  fb->fillRect(0, 196, 240, 44, CLAUDE_DARK);
  uint16_t c = seen ? CLAUDE_GREEN : aiPerson > 0.4f ? CLAUDE_AMBER : CLAUDE_GRAY;
  fb->fillRoundRect(60, 204, 172, 10, 5, CLAUDE_TRACK);
  fb->fillRoundRect(60, 204, max(10, (int)(172 * aiPerson)), 10, 5, c);
  fb->setTextSize(1);
  fb->setTextColor(c);
  fb->setCursor(60, 220);
  fb->printf("%s  %d%%", seen ? "Person!" : "Nobody", (int)(aiPerson * 100));
  fb->setTextColor(CLAUDE_GRAY);
  fb->setCursor(150, 220);
  fb->printf("%lu ms, offline", aiMs);
  drawBot(fb, 10, 206, 4, seen ? FACE_HAPPY : FACE_NORMAL, seen && (millis() / 200) % 2 ? 2 : seen ? 1 : 0, 0);
  pycamera.blitFrame();
}

// ---- Scavenger Hunt: Claude writes riddles, you hide QR codes, friends race to find them ----
#define HUNT_MAX 8
const char *HUNT_THEMES[] = {"Pirate treasure", "Space mission", "Dinosaur dig", "Secret agents", "Birthday surprise", "Say a theme..."};
#define HUNT_THEME_COUNT 6
const char *HUNT_FALLBACK[8][2] = {
  {"I keep food cold but my light comes on when you visit.", "the fridge"},
  {"Four legs, no feet. Everyone gathers here to eat.", "under the dining table"},
  {"Rest your head and look beneath.", "under a pillow"},
  {"Plates and cups get a bath with me.", "by the kitchen sink"},
  {"Shoes come home and rest with me.", "the shoe rack"},
  {"I show your face each morning.", "the bathroom mirror"},
  {"Stories sleep on my shelves.", "the bookshelf"},
  {"Open my doors to find your clothes.", "the wardrobe"}};
String huntClue[HUNT_MAX], huntSpot[HUNT_MAX], huntTheme, huntMsg, huntId;
int huntCount = 5, huntThemeSel = 0, huntIdx = 0, huntShow = 0;
int huntState = 0; // 0 setup, 1 asking Claude, 2 hide the codes, 3 playing, 4 scanning, 5 finished
unsigned long huntStart = 0, huntCheerUntil = 0, huntTime = 0, lastHuntScan = 0;
bool huntScanning = false;
String huntCode(int i) {
  return "CLAWD-HUNT:" + huntId + ":" + String(i);
}
void huntUseFallback() {
  for (int i = 0; i < huntCount; i++) {
    huntClue[i] = HUNT_FALLBACK[i][0];
    huntSpot[i] = HUNT_FALLBACK[i][1];
  }
}
void huntBegin() { // new codes every game, so last time's QR codes don't count
  char id[5];
  snprintf(id, sizeof(id), "%04X", (unsigned)(esp_random() & 0xFFFF));
  huntId = id;
  huntShow = 0;
  huntState = 2;
}
void huntRequest() {
  huntState = 1;
  huntMsg = "";
  if (!waitNetIdle(4000)) {
    huntUseFallback();
    huntMsg = "Phone busy: using built-in clues";
    return huntBegin();
  }
  JsonDocument req;
  req["theme"] = huntTheme;
  req["count"] = huntCount;
  size_t len = measureJson(req);
  uint8_t *body = (uint8_t *)ps_malloc(len + 1);
  if (body) serializeJson(req, (char *)body, len + 1);
  if (!body || !startNetJob(JOB_HUNT, "/hunt", body, len, "application/json")) {
    free(body);
    huntUseFallback();
    huntMsg = "No phone: using built-in clues";
    huntBegin();
  }
}
void handleHuntResult(bool parsed, JsonDocument &doc) {
  int n = 0;
  if (parsed && (doc["ok"] | false)) {
    for (JsonObject c : doc["clues"].as<JsonArray>()) {
      if (n >= huntCount) break;
      huntClue[n] = (const char *)(c["clue"] | "");
      huntSpot[n++] = (const char *)(c["spot"] | "");
    }
  }
  if (n < huntCount) {
    huntUseFallback();
    huntMsg = parsed ? String("Claude said: ") + (const char *)(doc["error"] | "no clues") + " - built-in clues" : String("No phone: using built-in clues");
  }
  huntBegin();
}
void handleHuntQr(bool parsed, JsonDocument &doc) {
  if (!parsed) return;
  String text = (const char *)(doc["text"] | "");
  if (!text.startsWith("CLAWD-HUNT:")) return;
  if (!text.startsWith("CLAWD-HUNT:" + huntId + ":")) {
    huntMsg = "That code is from another hunt";
    return;
  }
  int i = text.substring(text.lastIndexOf(':') + 1).toInt();
  if (i != huntIdx) {
    huntMsg = "That's code #" + String(i + 1) + " - find #" + String(huntIdx + 1) + " first!";
    return;
  }
  huntIdx++;
  huntScanning = false;
  huntCheerUntil = millis() + 1800;
  pycamera.speaker_tone(1046, 60);
  pycamera.speaker_tone(1568, 90);
  petBoost(5);
  if (huntIdx >= huntCount) {
    huntTime = millis() - huntStart;
    huntState = 5;
    uint32_t best = prefs.getUInt("hunt_best", 0);
    if (!best || huntTime / 1000 < best) prefs.putUInt("hunt_best", huntTime / 1000);
  } else {
    huntState = 3;
    huntMsg = "";
  }
}
void huntTool() {
  unsigned long now = millis();
  switch (huntState) {
    case 0: // pick a theme and how many clues
      if (pycamera.justPressed(AWEXP_BUTTON_UP)) huntThemeSel = (huntThemeSel + HUNT_THEME_COUNT - 1) % HUNT_THEME_COUNT;
      if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) huntThemeSel = (huntThemeSel + 1) % HUNT_THEME_COUNT;
      if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) huntCount = max(3, huntCount - 1);
      if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) huntCount = min(HUNT_MAX, huntCount + 1);
      if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
        if (huntThemeSel == HUNT_THEME_COUNT - 1) {
          if (waitNetIdle(4000)) {
            listenPurpose = LP_HUNT;
            if (startNetJob(JOB_LISTEN2, "/listen", NULL, 0, "")) huntState = 1;
          }
        } else {
          huntTheme = HUNT_THEMES[huntThemeSel];
          huntRequest();
        }
      }
      break;
    case 2: // show each QR code and where to hide it
      if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) huntShow = (huntShow + huntCount - 1) % huntCount;
      if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) huntShow = (huntShow + 1) % huntCount;
      if (pycamera.justPressed(AWEXP_BUTTON_OK) || pycamera.justPressed(SHUTTER_BUTTON)) {
        huntIdx = 0;
        huntStart = now;
        huntState = 3;
        huntMsg = "";
      }
      break;
    case 3: // read the clue, then scan
      if (pycamera.justPressed(AWEXP_BUTTON_OK) || pycamera.justPressed(SHUTTER_BUTTON)) {
        huntState = 4;
        huntScanning = true;
        huntMsg = "";
      }
      break;
    case 4: // scanning with the camera (the phone reads the code)
      if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) {
        huntState = 3;
        huntScanning = false;
      }
      break;
    case 5:
      if (pycamera.justPressed(AWEXP_BUTTON_OK)) huntState = 0;
      break;
  }
  if (huntState == 4) {
    pycamera.captureFrame();
    if (!netBusy() && pycamera.frame && now - lastHuntScan > 250) {
      size_t len = pycamera.frame->len;
      uint8_t *jpg = (uint8_t *)ps_malloc(len);
      if (jpg) {
        memcpy(jpg, pycamera.frame->buf, len);
        if (startNetJob(JOB_QR, "/qr", jpg, len, "image/jpeg")) lastHuntScan = now;
        else free(jpg);
      }
    }
    PyCameraFB *fb = pycamera.fb;
    drawCameraTitle(fb, ("Find code #" + String(huntIdx + 1) + " of " + String(huntCount)).c_str());
    for (int k = 0; k < 3; k++) fb->drawRoundRect(50 - k, 50 - k, 140 + 2 * k, 140 + 2 * k, 10, CLAUDE_ORANGE);
    drawCameraHint(fb, huntMsg.length() ? huntMsg : String("Point at the QR code   < back"), huntMsg.length() ? CLAUDE_AMBER : CLAUDE_CREAM);
    pycamera.blitFrame();
    return;
  }
  static unsigned long lastDraw = 0;
  if (now - lastDraw < 66) return;
  lastDraw = now;
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print("Scavenger Hunt");
  if (huntState == 0) {
    for (int i = 0; i < HUNT_THEME_COUNT; i++) {
      int y = 36 + i * 26;
      bool sel = i == huntThemeSel;
      if (sel) ui->fillRoundRect(10, y, 220, 22, 8, CLAUDE_ORANGE);
      else ui->drawRoundRect(10, y, 220, 22, 8, CLAUDE_TRACK);
      ui->setTextSize(1);
      ui->setTextColor(sel ? CLAUDE_DARK : CLAUDE_CREAM);
      ui->setCursor(20, y + 7);
      ui->print(HUNT_THEMES[i]);
    }
    drawCentered(String("< ") + huntCount + " clues >", 198, 2, CLAUDE_ORANGE);
    uint32_t best = prefs.getUInt("hunt_best", 0);
    drawCentered(best ? "OK: Claude writes clues   best " + String(best / 60) + "m" + String(best % 60) + "s" : String("OK: Claude writes the clues"), 226, 1, CLAUDE_GRAY);
  } else if (huntState == 1) {
    drawClaudeLogo(ui, 120, 110, 26, now * 0.3f, CLAUDE_ORANGE);
    drawCentered(listenPurpose == LP_HUNT && netJob == JOB_LISTEN2 ? "Say a theme to your phone..." : "Claude is writing riddles...", 160, 1, CLAUDE_CREAM);
  } else if (huntState == 2) {
    drawQrCode(huntCode(huntShow), 120, 104, 130);
    drawCentered("Code #" + String(huntShow + 1) + " of " + String(huntCount) + ": hide it at", 176, 1, CLAUDE_GRAY);
    drawCentered(huntSpot[huntShow], 190, 1, CLAUDE_ORANGE);
    if (WiFi.status() == WL_CONNECTED) drawCentered("Print all: http://" + WiFi.localIP().toString() + "/hunt", 206, 1, CLAUDE_GRAY);
    drawCentered("< > codes   OK: start the hunt", 226, 1, CLAUDE_GRAY);
  } else if (huntState == 3) {
    if (now < huntCheerUntil) { // confetti for the find
      for (int k = 0; k < 40; k++) {
        uint32_t h = k * 2654435761u;
        int x = h % 240, y = ((h >> 8) % 240 + (now - huntCheerUntil + 1800) / 4) % 240;
        ui->fillRect(x, y, 3, 3, k % 3 ? CLAUDE_ORANGE : CLAUDE_CREAM);
      }
      drawCentered("Found it!", 40, 2, CLAUDE_GREEN);
    }
    drawCentered("Clue " + String(huntIdx + 1) + " of " + String(huntCount), 64, 1, CLAUDE_GRAY);
    String lines[6];
    int n = wrapText(huntClue[huntIdx], 18, lines, 6);
    for (int i = 0; i < n; i++) drawCentered(lines[i], 84 + i * 20, 2, CLAUDE_CREAM);
    unsigned long t = (now - huntStart) / 1000;
    drawCentered(String(t / 60) + ":" + (t % 60 < 10 ? "0" : "") + String(t % 60), 204, 1, CLAUDE_ORANGE);
    drawCentered(huntMsg.length() ? huntMsg : String("Found the code? OK: scan it"), 226, 1, huntMsg.length() ? CLAUDE_AMBER : CLAUDE_GRAY);
  } else if (huntState == 5) {
    for (int k = 0; k < 60; k++) {
      uint32_t h = k * 2654435761u;
      ui->fillRect(h % 240, ((h >> 8) % 240 + now / 5) % 240, 3, 3, k % 3 ? CLAUDE_ORANGE : CLAUDE_AMBER);
    }
    drawBot(ui, 71, 90, 14, FACE_HAPPY, (now / 200) % 2 ? 2 : 1, (now / 200) % 2 ? 1 : 2);
    drawCentered("Hunt complete!", 44, 2, CLAUDE_ORANGE);
    unsigned long t = huntTime / 1000;
    drawCentered(String(huntCount) + " clues in " + String(t / 60) + "m " + String(t % 60) + "s", 176, 2, CLAUDE_CREAM);
    drawCentered("OK: new hunt", 226, 1, CLAUDE_GRAY);
  }
  if (huntMsg.length() && huntState == 2) drawCentered(huntMsg.substring(0, 38), 160, 1, CLAUDE_AMBER);
  pushUi();
}
void handleListen2Result(bool parsed, JsonDocument &doc) {
  String text = parsed && (doc["ok"] | false) ? String((const char *)(doc["text"] | "")) : String("");
  String err = parsed ? String((const char *)(doc["error"] | "I didn't catch that")) : phoneError(netStatus);
  if (listenPurpose == LP_BROWSER) {
    if (text.length()) browserOpen(text, true);
    else {
      brError = err;
      brState = brLineCount ? 2 : 0;
    }
  } else if (listenPurpose == LP_HUNT) {
    if (text.length()) {
      huntTheme = text;
      huntRequest();
    } else {
      huntMsg = err;
      huntState = 0;
    }
  }
  listenPurpose = 0;
}

// ---- Music Player: MP3 and WAV files from the SD card, on the board's speaker ----
// The speaker pin is driven as a 1-bit "PDM DAC" by the I2S peripheral, which is the closest
// this chip gets to real audio out. Expect small-speaker sound: clear tunes, little bass.
#define MUSIC_DIR "/music"
#define MUSIC_MAX 60
String musicFiles[MUSIC_MAX], musicError, musicPath;
int musicCount = 0, musicSel = 0, musicNow = -1, musicVolume = 3; // volume 1-4
volatile bool musicPlaying = false, musicPaused = false, musicStopReq = false, musicEnded = false;
volatile int musicPeak = 0;
volatile uint32_t musicSamples = 0, musicRate = 22050;
TaskHandle_t musicTaskHandle = NULL;
i2s_chan_handle_t musicTx = NULL;
bool musicScanned = false;
bool hasExt(const String &name, const char *ext) {
  String n = name;
  n.toLowerCase();
  return n.endsWith(ext);
}
void musicScan() {
  musicCount = 0;
  musicScanned = true;
  if (!pycamera.sd.exists(MUSIC_DIR)) pycamera.sd.mkdir(MUSIC_DIR);
  File dir = pycamera.sd.open(MUSIC_DIR);
  if (!dir) return;
  File f;
  char name[96];
  while (musicCount < MUSIC_MAX && f.openNext(&dir, O_RDONLY)) {
    f.getName(name, sizeof(name));
    String n = name;
    if (!f.isDir() && (hasExt(n, ".mp3") || hasExt(n, ".wav")) && !n.startsWith(".")) musicFiles[musicCount++] = n;
    f.close();
  }
  dir.close();
  for (int i = 1; i < musicCount; i++) // alphabetical
    for (int j = i; j > 0 && musicFiles[j] < musicFiles[j - 1]; j--) {
      String t = musicFiles[j];
      musicFiles[j] = musicFiles[j - 1];
      musicFiles[j - 1] = t;
    }
}
bool musicOutStart(uint32_t rate) {
  stopDance();
  noTone(BOARD_SPEAKER_PIN); // the beeps use the same pin; take it over
  delay(20);
  ledcDetach(BOARD_SPEAKER_PIN);
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true;
  if (i2s_new_channel(&cc, &musicTx, NULL) != ESP_OK) return false;
  i2s_pdm_tx_config_t cfg = {};
  i2s_pdm_tx_clk_config_t clk = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(rate);
  i2s_pdm_tx_slot_config_t slot = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  cfg.clk_cfg = clk;
  cfg.slot_cfg = slot;
  cfg.gpio_cfg.clk = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.dout = (gpio_num_t)BOARD_SPEAKER_PIN;
#if SOC_I2S_PDM_MAX_TX_LINES > 1
  cfg.gpio_cfg.dout2 = I2S_GPIO_UNUSED;
#endif
  if (i2s_channel_init_pdm_tx_mode(musicTx, &cfg) != ESP_OK || i2s_channel_enable(musicTx) != ESP_OK) {
    i2s_del_channel(musicTx);
    musicTx = NULL;
    return false;
  }
  musicRate = rate;
  pycamera.aw.digitalWrite(AWEXP_SPKR_SD, HIGH); // un-mute
  return true;
}
void musicSetRate(uint32_t rate) {
  if (!musicTx || rate == musicRate || rate < 8000 || rate > 48000) return;
  i2s_channel_disable(musicTx);
  i2s_pdm_tx_clk_config_t clk = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(rate);
  i2s_channel_reconfig_pdm_tx_clock(musicTx, &clk);
  i2s_channel_enable(musicTx);
  musicRate = rate;
}
void musicOutStop() {
  if (musicTx) {
    i2s_channel_disable(musicTx);
    i2s_del_channel(musicTx);
    musicTx = NULL;
  }
  gpio_reset_pin((gpio_num_t)BOARD_SPEAKER_PIN);
  pycamera.aw.digitalWrite(AWEXP_SPKR_SD, LOW);
}
// Volume, peak meter, then out to the speaker. Returns false when asked to stop.
bool musicWrite(int16_t *pcm, int n) {
  while (musicPaused && !musicStopReq) vTaskDelay(pdMS_TO_TICKS(20));
  if (musicStopReq) return false;
  int peak = 0;
  for (int i = 0; i < n; i++) {
    int s = pcm[i] * musicVolume / 4;
    pcm[i] = s;
    peak = max(peak, abs(s));
  }
  musicPeak = peak;
  size_t written;
  i2s_channel_write(musicTx, pcm, n * 2, &written, portMAX_DELAY);
  musicSamples += n;
  return true;
}
void musicPlayWav(File &f, int16_t *pcm, uint8_t *raw) {
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return;
  int channels = 1, bits = 16;
  uint32_t rate = 22050;
  while (f.available()) { // walk the chunks to "fmt " and "data"
    uint8_t ch[8];
    if (f.read(ch, 8) != 8) return;
    uint32_t size = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
    if (!memcmp(ch, "fmt ", 4)) {
      uint8_t fmt[16];
      f.read(fmt, 16);
      channels = fmt[2];
      rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16;
      bits = fmt[14];
      if (size > 16) f.seekCur(size - 16);
    } else if (!memcmp(ch, "data", 4)) {
      break;
    } else {
      f.seekCur(size + (size & 1));
    }
  }
  if ((bits != 8 && bits != 16) || channels < 1 || channels > 2) return;
  musicSetRate(rate);
  int frameBytes = channels * bits / 8;
  while (true) {
    int got = f.read(raw, 1024 * frameBytes);
    int frames = got / frameBytes;
    if (frames <= 0) return;
    for (int i = 0; i < frames; i++) { // to 16-bit mono
      int32_t s = 0;
      for (int c = 0; c < channels; c++)
        s += bits == 16 ? (int16_t)(raw[(i * channels + c) * 2] | raw[(i * channels + c) * 2 + 1] << 8)
                        : ((int)raw[i * channels + c] - 128) << 8;
      pcm[i] = s / channels;
    }
    if (!musicWrite(pcm, frames)) return;
  }
}
void musicPlayMp3(File &f, int16_t *pcm, uint8_t *in) {
  HMP3Decoder dec = MP3InitDecoder();
  if (!dec) return;
  uint8_t id3[10];
  if (f.read(id3, 10) == 10 && !memcmp(id3, "ID3", 3)) { // skip the ID3v2 tag
    uint32_t size = (id3[6] & 0x7F) << 21 | (id3[7] & 0x7F) << 14 | (id3[8] & 0x7F) << 7 | (id3[9] & 0x7F);
    f.seekSet(10 + size);
  } else {
    f.seekSet(0);
  }
  int left = 0;
  uint8_t *ptr = in;
  int16_t stereo[1152 * 2];
  while (true) {
    if (left < 2048) { // top up the input buffer
      memmove(in, ptr, left);
      int got = f.read(in + left, 4096 - left);
      if (got > 0) left += got;
      ptr = in;
      if (left <= 0) break;
    }
    int off = MP3FindSyncWord(ptr, left);
    if (off < 0) {
      left = 0;
      continue;
    }
    ptr += off;
    left -= off;
    int err = MP3Decode(dec, &ptr, &left, stereo, 0);
    if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
      if (!f.available()) break;
      continue;
    }
    if (err) { // corrupt frame: skip a byte and resync
      if (left > 0) {
        ptr++;
        left--;
      }
      continue;
    }
    MP3FrameInfo info;
    MP3GetLastFrameInfo(dec, &info);
    musicSetRate(info.samprate);
    int frames = info.outputSamps / max(1, info.nChans);
    for (int i = 0; i < frames; i++) pcm[i] = info.nChans == 2 ? (stereo[i * 2] + stereo[i * 2 + 1]) / 2 : stereo[i];
    if (!musicWrite(pcm, frames)) break;
  }
  MP3FreeDecoder(dec);
}
void musicTask(void *) {
  int16_t *pcm = (int16_t *)malloc(1152 * sizeof(int16_t) * 2);
  uint8_t *buf = (uint8_t *)malloc(4096);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    File f = pycamera.sd.open(musicPath.c_str(), FILE_READ);
    if (f && pcm && buf) {
      musicSamples = 0;
      if (hasExt(musicPath, ".wav")) musicPlayWav(f, pcm, buf);
      else musicPlayMp3(f, pcm, buf);
    }
    f.close();
    musicEnded = !musicStopReq;
    musicPlaying = false;
  }
}
void musicPlay(int i) {
  musicStop();
  if (i < 0 || i >= musicCount) return;
  if (!musicTx && !musicOutStart(22050)) {
    musicError = "Couldn't start the speaker";
    return;
  }
  if (!musicTaskHandle) xTaskCreatePinnedToCore(musicTask, "music", 12288, NULL, 2, &musicTaskHandle, 0);
  musicNow = i;
  musicPath = String(MUSIC_DIR) + "/" + musicFiles[i];
  musicStopReq = false;
  musicPaused = false;
  musicEnded = false;
  musicPlaying = true;
  musicError = "";
  xTaskNotifyGive(musicTaskHandle);
}
void musicStop() {
  if (musicPlaying) {
    musicStopReq = true;
    musicPaused = false;
    unsigned long t = millis();
    while (musicPlaying && millis() - t < 1000) delay(5);
  }
  if (musicTx) musicOutStop();
}
void musicMode() {
  if (!musicScanned) musicScan();
  if (musicEnded) { // next track
    musicEnded = false;
    if (musicCount) musicPlay((musicNow + 1) % musicCount);
  }
  if (pycamera.justPressed(AWEXP_BUTTON_UP) && musicCount) musicSel = (musicSel + musicCount - 1) % musicCount;
  if (pycamera.justPressed(AWEXP_BUTTON_DOWN) && musicCount) musicSel = (musicSel + 1) % musicCount;
  if (pycamera.justPressed(AWEXP_BUTTON_OK) && musicCount) {
    if (musicPlaying && musicSel == musicNow) musicPaused = !musicPaused;
    else musicPlay(musicSel);
  }
  if (pycamera.justPressed(AWEXP_BUTTON_RIGHT) && musicCount) musicPlay(musicSel = (max(0, musicNow) + 1) % musicCount);
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT) && musicCount) musicPlay(musicSel = (max(0, musicNow) + musicCount - 1) % musicCount);
  if (pycamera.justPressed(SHUTTER_BUTTON)) musicVolume = musicVolume % 4 + 1;
  static unsigned long lastDraw = 0;
  unsigned long now = millis();
  if (now - lastDraw < 50) return;
  lastDraw = now;
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, musicPlaying && !musicPaused ? now * 0.2f : 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print("Music");
  for (int v = 0; v < 4; v++) ui->fillRect(196 + v * 8, 22 - v * 4, 5, 4 + v * 4, v < musicVolume ? CLAUDE_ORANGE : CLAUDE_TRACK);
  if (!musicCount) {
    drawMusicNote(108, 110, 2, CLAUDE_TRACK);
    drawCentered("Put .mp3 or .wav files in", 140, 1, CLAUDE_CREAM);
    drawCentered("/music on the SD card", 152, 1, CLAUDE_ORANGE);
    drawCentered("Mono, 22-44 kHz plays best", 170, 1, CLAUDE_GRAY);
    if (pycamera.justPressed(AWEXP_BUTTON_OK)) musicScan();
    pushUi();
    return;
  }
  // now playing: Clawd bobs to the loudness, with a level meter
  bool live = musicPlaying && !musicPaused;
  int level = live ? min(100, musicPeak * 100 / 20000) : 0;
  drawBot(ui, 16, 48 - level / 12, 6, live ? FACE_HAPPY : FACE_BLINK, level > 50 ? 2 : 1, level > 70 ? 2 : 0);
  for (int b = 0; b < 12; b++) {
    int h = live ? max(2, (int)(level * (0.4f + 0.6f * fabsf(sinf(now / 90.0f + b * 1.3f))) * 40 / 100)) : 2;
    ui->fillRect(78 + b * 13, 80 - h, 9, h, b < 8 ? CLAUDE_ORANGE : b < 10 ? CLAUDE_AMBER : CLAUDE_RED);
  }
  if (musicNow >= 0) {
    String t = musicFiles[musicNow];
    t = t.substring(0, t.lastIndexOf('.'));
    if (t.length() > 38) t = t.substring(0, 37) + ".";
    drawCentered(t, 88, 1, musicPaused ? CLAUDE_GRAY : CLAUDE_CREAM);
    uint32_t secs = musicSamples / max((uint32_t)1, (uint32_t)musicRate);
    drawCentered(String(musicPaused ? "paused " : "") + String(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + String(secs % 60), 100, 1, CLAUDE_GRAY);
  }
  int first = constrain(musicSel - 3, 0, max(0, musicCount - 7));
  for (int i = first; i < first + 7 && i < musicCount; i++) {
    int y = 114 + (i - first) * 15;
    if (i == musicSel) ui->fillRoundRect(4, y - 2, 232, 13, 4, CLAUDE_ORANGE);
    ui->setTextSize(1);
    ui->setTextColor(i == musicSel ? CLAUDE_DARK : i == musicNow ? CLAUDE_ORANGE : CLAUDE_CREAM);
    ui->setCursor(10, y);
    String n = musicFiles[i];
    ui->print(n.length() > 37 ? n.substring(0, 36) + "." : n);
  }
  if (musicError.length()) drawCentered(musicError, 214, 1, CLAUDE_RED);
  drawCentered("OK play/pause  < > track  Shutter vol", 228, 1, CLAUDE_GRAY);
  pushUi();
}

// ---- Web pages for Retro Cam photos and the hunt's printable QR codes ----
String qrSvgOut;
void qrToSvg(esp_qrcode_handle_t q) {
  int n = esp_qrcode_get_size(q);
  qrSvgOut = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='-2 -2 " + String(n + 4) + " " + String(n + 4) +
             "' width='200' height='200' shape-rendering='crispEdges'><rect x='-2' y='-2' width='100%' height='100%' fill='#fff'/><path d='";
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (esp_qrcode_get_module(q, x, y)) qrSvgOut += "M" + String(x) + " " + String(y) + "h1v1h-1z";
  qrSvgOut += "'/></svg>";
}
String qrSvg(const String &text) {
  qrSvgOut = "";
  esp_qrcode_config_t cfg = {qrToSvg, 10, ESP_QRCODE_ECC_LOW};
  esp_qrcode_generate(&cfg, text.c_str());
  return qrSvgOut;
}
void webStreamFile(WiFiClient &c, const String &path) {
  File f = pycamera.sd.open(path.c_str(), FILE_READ);
  if (!f) {
    webReply(c, 404, "text/plain", "not found");
    return;
  }
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
           hasExt(path, ".gif") ? "image/gif" : "image/jpeg", (unsigned long)f.size());
  uint8_t buf[1024];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) c.write(buf, n);
  f.close();
}
// Extra routes for the web remote; returns true if it answered
bool webExtraRoute(WiFiClient &c, const String &path, const String &query) {
  if (path == "/rc") {
    String name = queryArg(query, "f");
    if (!name.length() || name.indexOf('/') >= 0 || name.indexOf("..") >= 0) webReply(c, 400, "text/plain", "bad name");
    else webStreamFile(c, "/retrocam/" + name);
    return true;
  }
  if (path == "/gallery") {
    String html = "<!doctype html><meta name=viewport content='width=device-width'><title>Retro Cam</title>"
                  "<body style='background:#141413;color:#faf9f5;font-family:sans-serif'><h2 style='color:#d97757'>Retro Cam</h2>";
    File dir = pycamera.sd.open("/retrocam");
    File f;
    char name[48];
    int shown = 0;
    while (dir && shown < 60 && f.openNext(&dir, O_RDONLY)) {
      f.getName(name, sizeof(name));
      f.close();
      if (!hasExt(name, ".jpg") && !hasExt(name, ".gif")) continue;
      html += "<a href='/rc?f=" + String(name) + "'><img src='/rc?f=" + String(name) +
              "' width=160 style='image-rendering:pixelated;margin:4px;border-radius:6px'></a>";
      shown++;
    }
    dir.close();
    if (!shown) html += "<p>No photos yet. Take some in Retro Cam!</p>";
    webReply(c, 200, "text/html", (html + "</body>").c_str());
    return true;
  }
  if (path == "/hunt") {
    String html = "<!doctype html><meta name=viewport content='width=device-width'><title>Scavenger Hunt codes</title>"
                  "<body style='font-family:sans-serif'><h2>Scavenger Hunt: print, cut out, hide</h2>";
    if (!huntId.length()) html += "<p>Start a hunt on the board first (Toolbox &gt; Scavenger Hunt).</p>";
    for (int i = 0; huntId.length() && i < huntCount; i++)
      html += "<div style='display:inline-block;margin:10px;text-align:center;border:1px dashed #999;padding:8px'>" + qrSvg(huntCode(i)) +
              "<br><b>#" + String(i + 1) + "</b><br><small>hide at: " + huntSpot[i] + "</small></div>";
    webReply(c, 200, "text/html", (html + "</body>").c_str());
    return true;
  }
  return false;
}
// Called when you leave a screen with SEL: stop anything running in the Fun Lab
void leaveFunLab() {
  if (rcGifFrames) {
    gifEnd();
    rcGifFrames = 0;
  }
  musicStop();
  duelLobby = false;
  huntScanning = false;
}

// ===================== BOOT QUOTE =====================
// Even the smallest LLMs need far more memory than this board has, so the bot
// picks a line from this hand-written list instead (works offline, costs nothing)
const char *const QUOTES[] = {
  "Small steps still move you forward.",
  "Done is better than perfect.",
  "Your future self is cheering for you.",
  "Every expert was once a beginner.",
  "Ship it, learn, repeat.",
  "Bugs are just puzzles in disguise.",
  "Progress, not perfection.",
  "Start where you are. Use what you have.",
  "Today is a great day to build something.",
  "Curiosity is your superpower.",
  "One commit at a time.",
  "Hard things get easier with practice.",
  "You've solved harder problems before.",
  "Make it work, then make it beautiful.",
  "Focus on the next right step.",
  "Rest is part of the work.",
  "Consistency beats intensity.",
  "Ask good questions. Build cool things.",
  "Mistakes mean you're trying.",
  "The best time to start is now.",
  "Stay curious, stay kind.",
  "Dream big, debug small.",
  "Little wins add up to big ones.",
  "It's okay to be a work in progress.",
  "Think it. Sketch it. Build it.",
  "Great things take a few tries.",
  "Breathe. Then fix the next bug.",
  "Learn one new thing today.",
  "Your ideas deserve to exist.",
  "Keep going. You're closer than you think.",
  "Simple is hard. It's worth it.",
  "Make today count.",
  "Be brave enough to be bad at something new.",
  "Every line of code is a step forward.",
  "Turn coffee into creations.",
  "Trust the process, check the output.",
};
void showBootQuote() {
  const int count = sizeof(QUOTES) / sizeof(QUOTES[0]);
  String quote = QUOTES[esp_random() % count];
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 18, 18, 9, 0, CLAUDE_ORANGE);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(34, 15);
  ui->print("Thought of the day");
  drawBubble(ui, 12, 38, 216, 120, 62, 168);
  String lines[6];
  int size = 2;
  int n = wrapText(quote, 16, lines, 6);
  if (n > 5) {
    size = 1;
    n = wrapText(quote, 32, lines, 6);
  }
  int lineH = size == 2 ? 20 : 12;
  int top = 40 + (120 - n * lineH) / 2; // centred in the bubble
  ui->setTextSize(size);
  ui->setTextColor(CLAUDE_DARK);
  for (int i = 0; i < n; i++) {
    ui->setCursor(120 - lines[i].length() * 3 * size, top + i * lineH);
    ui->print(lines[i]);
  }
  drawBot(ui, 40, 172, 8, FACE_HAPPY, 2, 0);
  pushUi();
}

// ===================== CLAWD TALK =====================
// Push-to-talk, like a Rabbit R1: press Shutter and talk to your phone (the board has no
// microphone, so the phone app listens). The answer types itself out here and the phone says it
// out loud. LEFT/RIGHT picks a mode:
//   Ask       anything; Claude can search the web and act: "wake me at 7", "focus for 25
//             minutes", "dance", "remind me to call mum in an hour", "play lo-fi on my phone"
//   Translate what you say, into the language picked with UP/DOWN
//   Note      saved to the notes in the phone app
#define TALK_MODES 3
const char *TALK_MODE_NAMES[] = {"Ask", "Translate", "Note"};
const char *TALK_MODE_KEYS[] = {"ask", "translate", "note"};
const char *TALK_HINTS[] = {"Ask anything. I can search the web, set alarms, start a focus timer or play music on your phone.",
                            "Say something and I'll translate it. The phone speaks it out loud.",
                            "Say a note and I'll keep it in the phone app."};
TalkState talkState = TALK_IDLE;
int talkMode = 0;
String talkHeard, talkRaw; // what you said: ASCII for the screen, as heard for Claude
String talkChip;           // what an action did, e.g. "Alarm 7:00 AM"
bool talkFailed = false, talkSendPending = false;
String talkLines[ASK_MAX_LINES];
int talkLineCount = 0, talkScroll = 0, talkTextSize = 2, talkReveal = 0;
unsigned long talkStateAt = 0, talkDanceAt = 0;
void setTalkState(TalkState s) {
  talkState = s;
  talkStateAt = millis();
}
void openTalk() {
  currentMode = MODE_TALK; // a question already on its way keeps going
}
void showTalkAnswer(const String &answer, bool failed) {
  talkFailed = failed;
  talkTextSize = 2;
  talkLineCount = wrapText(answer, 17, talkLines, ASK_MAX_LINES);
  if (talkLineCount > 6) { // too long for big text
    talkTextSize = 1;
    talkLineCount = wrapText(answer, 34, talkLines, ASK_MAX_LINES);
  }
  talkScroll = 0;
  talkReveal = 0;
  setTalkState(TALK_ANSWER);
  if (danceOn) return; // the dance owns the speaker
  if (failed) {
    pycamera.speaker_tone(300, 150);
  } else {
    pycamera.speaker_tone(1046, 60);
    pycamera.speaker_tone(1568, 90);
  }
}
void startTalk() {
  talkHeard = talkRaw = talkChip = "";
  if (WiFi.status() != WL_CONNECTED) return showTalkAnswer("No WiFi connection.", true);
  if (!waitNetIdle(4000) || !startNetJob(JOB_LISTEN, "/listen", NULL, 0, "")) {
    return showTalkAnswer("The phone link is busy. Try again.", true);
  }
  setTalkState(TALK_LISTENING);
  pycamera.speaker_tone(1320, 40); // "go ahead" chirp
  pycamera.speaker_tone(1760, 60);
  petBoost(2);
}
void handleListenResult(bool parsed, JsonDocument &doc) {
  if (!parsed) return showTalkAnswer(phoneError(netStatus), true);
  if (!(doc["ok"] | false)) return showTalkAnswer((const char *)(doc["error"] | "I didn't catch that."), true);
  talkHeard = (const char *)(doc["text"] | "");
  talkRaw = (const char *)(doc["raw"] | "");
  setTalkState(TALK_THINKING);
  talkSendPending = true; // sent from talkTick() once this job is cleared
  if (!danceOn) pycamera.speaker_tone(988, 30);
}
void handleTalkResult(bool parsed, JsonDocument &doc) {
  if (!parsed) return showTalkAnswer(phoneError(netStatus), true);
  bool ok = doc["ok"] | false;
  String chip;
  if (ok) runBoardActions(doc["actions"].as<JsonArray>(), &chip);
  talkChip = chip;
  showTalkAnswer((const char *)(doc["answer"] | (ok ? "Done." : "Something went wrong.")), !ok);
}
// Things Claude decided to do on the board. summary collects a short "what I did" line;
// without one (actions said into the phone app) the board just chirps and shows it in Talk.
void runBoardActions(JsonArray actions, String *summary) {
  for (JsonObject a : actions) {
    String type = a["type"] | "";
    String done;
    if (type == "alarm") {
      uint8_t h = constrain(a["hour"] | 7, 0, 23), m = constrain(a["minute"] | 0, 0, 59), r = constrain(a["repeat"] | 0, 0, 3);
      int slot = -1;
      for (int i = 0; i < MAX_ALARMS && slot < 0; i++)
        if (alarms[i].hour == h && alarms[i].minute == m) slot = i; // same time: just switch it on
      for (int i = 0; i < MAX_ALARMS && slot < 0; i++)
        if (!alarms[i].on) slot = i;
      if (slot < 0) slot = MAX_ALARMS - 1; // all four in use: replace the last
      alarms[slot] = {true, h, m, r};
      saveAlarms();
      done = "Alarm " + clockText(h, m);
    } else if (type == "focus") {
      pomoWorkMin = constrain(a["minutes"] | 25, 1, 120);
      pomoReset();
      pomoStart();
      done = "Focus " + String(pomoWorkMin) + " min";
    } else if (type == "dance") {
      talkDanceAt = millis() + 2500; // after the answer has had a moment on screen
      done = "Dance time!";
    } else if (type == "feed") {
      pet.food = min(100.0f, pet.food + 25);
      petBoost(5);
      savePet();
      done = "Clawd fed";
    }
    if (!done.length()) continue;
    if (summary) {
      if (summary->length()) *summary += "  ";
      *summary += done;
    } else {
      talkChip = done;
      if (!danceOn) pycamera.speaker_tone(1568, 40);
    }
  }
}
// Called every loop(), on any screen: sends what you said, and starts a requested dance
void talkTick() {
  if (talkSendPending && !netBusy()) {
    talkSendPending = false;
    JsonDocument req;
    req["prompt"] = talkHeard;
    req["raw"] = talkRaw;
    req["mode"] = TALK_MODE_KEYS[talkMode];
    req["lang"] = LANGS[langSel];
    req["source"] = "board";
    size_t len = measureJson(req);
    uint8_t *body = (uint8_t *)ps_malloc(len + 1);
    if (body) serializeJson(req, (char *)body, len + 1);
    if (!body || !startNetJob(JOB_TALK, "/ask/text", body, len, "application/json")) {
      free(body);
      showTalkAnswer("Lost the phone link. Try again.", true);
    }
  }
  if (talkDanceAt && millis() > talkDanceAt) {
    talkDanceAt = 0;
    if (currentMode != MODE_ALARM_RING) {
      if (currentMode == MODE_SLEEP) wakeUp();
      currentMode = MODE_DANCE;
      startDance(0);
      petBoost(5);
    }
  }
}
// ---- drawing
void drawTalkHeader() {
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 14, 14, 9, millis() * (talkState == TALK_THINKING ? 0.3f : 0.0f), CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(30, 7);
  ui->print("Clawd");
  drawPhoneChip(ui, 232, 8);
  for (int i = 0; i < TALK_MODES; i++) { // mode tabs
    int x = 6 + i * 77;
    bool on = i == talkMode;
    if (on) ui->fillRoundRect(x, 30, 74, 20, 10, CLAUDE_ORANGE);
    else ui->drawRoundRect(x, 30, 74, 20, 10, CLAUDE_TRACK);
    ui->setTextSize(1);
    ui->setTextColor(on ? CLAUDE_DARK : CLAUDE_GRAY);
    ui->setCursor(x + 37 - strlen(TALK_MODE_NAMES[i]) * 3, 36);
    ui->print(TALK_MODE_NAMES[i]);
  }
}
void drawTalkChip(int y) {
  if (!talkChip.length()) return;
  String chip = talkChip.length() > 36 ? talkChip.substring(0, 35) + "." : talkChip;
  int w = chip.length() * 6 + 16;
  ui->fillRoundRect(120 - w / 2, y, w, 15, 7, CLAUDE_ORANGE);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_DARK);
  ui->setCursor(120 - w / 2 + 8, y + 4);
  ui->print(chip);
}
// The orange push-to-talk pill at the bottom
void drawTalkButton(const char *text, bool lit) {
  int w = strlen(text) * 6 + 28;
  if (lit) ui->fillRoundRect(120 - w / 2, 214, w, 22, 11, CLAUDE_ORANGE);
  else ui->drawRoundRect(120 - w / 2, 214, w, 22, 11, CLAUDE_ORANGE);
  ui->setTextSize(1);
  ui->setTextColor(lit ? CLAUDE_DARK : CLAUDE_ORANGE);
  ui->setCursor(120 - strlen(text) * 3, 221);
  ui->print(text);
}
void drawTalkIdle(unsigned long now) {
  int botY = 76;
  if (talkMode == 1) { // the language to translate into, changed with UP/DOWN
    String lang = LANGS[langSel];
    drawCentered("into", 58, 1, CLAUDE_GRAY);
    drawCentered(lang, 70, 2, CLAUDE_ORANGE);
    int half = lang.length() * 6 + 10;
    ui->fillTriangle(120 - half - 8, 77, 120 - half, 71, 120 - half, 83, CLAUDE_TRACK);
    ui->fillTriangle(120 + half + 8, 77, 120 + half, 71, 120 + half, 83, CLAUDE_TRACK);
    botY = 98;
  }
  Face face = usage.error.length() ? FACE_WORRIED : FACE_NORMAL;
  if (now % 3500 < 140) face = FACE_BLINK;
  int bob = (int)(2 * sinf(now / 600.0f));
  drawBot(ui, 85, botY + bob, 10, face, 0, 0);
  int y = botY + 62;
  if (talkChip.length()) {
    drawTalkChip(y);
    y += 20;
  }
  bool problem = usage.error.length() > 0;
  String hint = problem ? usage.error : String(TALK_HINTS[talkMode]);
  String lines[3];
  int n = wrapText(hint, 36, lines, min(3, (210 - y) / 11));
  ui->setTextSize(1);
  ui->setTextColor(problem ? CLAUDE_AMBER : CLAUDE_GRAY);
  for (int i = 0; i < n; i++) {
    ui->setCursor(120 - lines[i].length() * 3, y + i * 11);
    ui->print(lines[i]);
  }
  drawTalkButton("SHUTTER  TO TALK", true);
}
void drawTalkListening(unsigned long now) {
  unsigned long t = now - talkStateAt;
  drawBot(ui, 85, 66, 10, t % 2600 < 140 ? FACE_BLINK : FACE_HAPPY, 1, 1); // ears up, all attention
  // sound bars bouncing like a voice meter
  for (int i = 0; i < 15; i++) {
    float wave = sinf(t / 90.0f + i * 0.7f) * 0.5f + 0.5f;
    float swell = sinf(t / 400.0f + i * 0.3f) * 0.5f + 0.5f;
    int h = 4 + (int)(26 * wave * (0.35f + 0.65f * swell));
    ui->fillRoundRect(36 + i * 12, 150 - h / 2, 6, h, 3, CLAUDE_ORANGE);
  }
  drawCentered("Listening", 176, 2, CLAUDE_CREAM);
  drawCentered("Talk to your phone", 196, 1, CLAUDE_GRAY);
  char secs[8];
  snprintf(secs, sizeof(secs), "%lus", t / 1000);
  drawTalkButton(secs, false);
}
void drawTalkThinking(unsigned long now) {
  unsigned long t = now - talkStateAt;
  if (talkHeard.length()) {
    drawCentered("You said", 58, 1, CLAUDE_GRAY);
    String lines[2];
    int n = wrapText("\"" + talkHeard + "\"", 36, lines, 2);
    for (int i = 0; i < n; i++) drawCentered(lines[i], 72 + i * 11, 1, CLAUDE_CREAM);
  }
  float pulse = 1.0f + 0.12f * sinf(t / 180.0f);
  drawClaudeLogo(ui, 120, 132, 26 * pulse, t * 0.25f, CLAUDE_ORANGE);
  const char *verb = talkMode == 1 ? "Translating" : talkMode == 2 ? "Saving" : t < 4000 ? "Thinking" : thinkingQuips[(t / 2200) % 6];
  String label = String(verb);
  for (int i = 0; i < 3; i++) label += i < (int)((t / 350) % 4) ? '.' : ' ';
  drawCentered(label, 176, 2, CLAUDE_CREAM);
  char secs[8];
  snprintf(secs, sizeof(secs), "%lus", t / 1000);
  drawTalkButton(secs, false);
}
void drawTalkAnswer() {
  int top = 56;
  if (talkHeard.length()) {
    String line = "> " + talkHeard;
    if (line.length() > 38) line = line.substring(0, 37) + ".";
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(8, top);
    ui->print(line);
    top += 14;
  }
  int bottom = talkChip.length() ? 190 : 208;
  drawBubble(ui, 6, top, 228, bottom - top, 20, top - 6);
  int lineH = talkTextSize == 2 ? 18 : 11;
  int visible = (bottom - top - 10) / lineH;
  ui->setTextSize(talkTextSize);
  ui->setTextColor(talkFailed ? CLAUDE_RED : CLAUDE_DARK);
  int budget = talkReveal;
  for (int i = talkScroll; i < talkLineCount && i < talkScroll + visible && budget > 0; i++) {
    ui->setCursor(16, top + 8 + (i - talkScroll) * lineH);
    String &line = talkLines[i];
    ui->print(budget >= (int)line.length() ? line : line.substring(0, budget));
    budget -= line.length();
  }
  if (talkScroll > 0) ui->fillTriangle(222, top + 6, 216, top + 14, 228, top + 14, CLAUDE_ORANGE);
  if (talkScroll + visible < talkLineCount) ui->fillTriangle(222, bottom - 6, 216, bottom - 14, 228, bottom - 14, CLAUDE_ORANGE);
  drawTalkChip(194);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(6, 228);
  ui->print(talkScroll + visible < talkLineCount || talkScroll > 0 ? "^v more" : "");
  ui->setCursor(150, 228);
  ui->print("Shutter: talk");
}
void talkScreen() {
  bool waiting = talkState == TALK_LISTENING || talkState == TALK_THINKING;
  int lineH = talkTextSize == 2 ? 18 : 11;
  int visible = ((talkChip.length() ? 190 : 208) - (talkHeard.length() ? 70 : 56) - 10) / lineH;
  if (!waiting) {
    if (pycamera.justPressed(SHUTTER_BUTTON) || pycamera.justPressed(AWEXP_BUTTON_OK)) {
      startTalk();
      return;
    }
    if (pycamera.justPressed(AWEXP_BUTTON_LEFT) || pycamera.justPressed(AWEXP_BUTTON_RIGHT)) {
      talkMode = (talkMode + (pycamera.justPressed(AWEXP_BUTTON_LEFT) ? TALK_MODES - 1 : 1)) % TALK_MODES;
      talkChip = "";
      setTalkState(TALK_IDLE);
      pycamera.speaker_tone(1200, 15);
    }
    bool up = pycamera.justPressed(AWEXP_BUTTON_UP), down = pycamera.justPressed(AWEXP_BUTTON_DOWN);
    if (talkState == TALK_ANSWER && (up || down)) {
      if (down && talkScroll + visible < talkLineCount) talkScroll++;
      if (up && talkScroll > 0) talkScroll--;
      talkReveal = 9999;
    } else if (talkState == TALK_IDLE && talkMode == 1 && (up || down)) {
      langSel = (langSel + (up ? LANG_COUNT - 1 : 1)) % LANG_COUNT;
      prefs.putUChar("lang", langSel);
    }
  }
  static unsigned long lastDraw = 0;
  unsigned long now = millis();
  if (now - lastDraw < 33) return; // ~30 fps for the voice bars
  lastDraw = now;
  if (talkState == TALK_ANSWER && talkReveal < 9999) { // typewriter: ~4 characters per frame
    int total = 0;
    for (int i = 0; i < talkLineCount; i++) total += talkLines[i].length();
    talkReveal = talkReveal + 4 >= total ? 9999 : talkReveal + 4;
  }
  drawTalkHeader();
  switch (talkState) {
    case TALK_IDLE: drawTalkIdle(now); break;
    case TALK_LISTENING: drawTalkListening(now); break;
    case TALK_THINKING: drawTalkThinking(now); break;
    case TALK_ANSWER: drawTalkAnswer(); break;
  }
  pushUi();
}

// ===================== IDLE CLOCK =====================
// After a while with no button presses the screen dims and the bot dozes off under
// a clock. Any button wakes it back to whatever you were doing.
const unsigned long IDLE_AFTER_MS = 40000; // 40 seconds
#define BACKLIGHT_LEDC_CHANNEL 4
Mode modeBeforeSleep = MODE_USAGE;
bool canSleepNow() {
  switch (currentMode) {
    case MODE_HOME_ASSISTANT: // keeps watching the camera and Telegram
    case MODE_DANCE:          // music is playing
    case MODE_ALARM_RING:
    case MODE_FACE:           // hands-free
    case MODE_SLEEP:
      return false;
    case MODE_ASK:
      return askState != ASK_THINKING;
    case MODE_TALK:
      return talkState == TALK_IDLE || talkState == TALK_ANSWER;
    case MODE_GAME:           // Tilt Catch is played without buttons; Beats and Duel run on their own
      return gameOver || (currentGame != GAME_TILT && currentGame != GAME_BEATS && currentGame != GAME_DUEL);
    case MODE_TOOL:           // hands-free tools, a running timer, or a recording
      return !((currentTool == TOOL_POMODORO && pomoRunning) || currentTool == TOOL_LEVEL || currentTool == TOOL_QR ||
               currentTool == TOOL_REMOTE || (currentTool == TOOL_TIMELAPSE && tlState != 0) || currentTool == TOOL_AI ||
               (currentTool == TOOL_HUNT && (huntState == 3 || huntState == 4)));
    case MODE_MUSIC:
      return !musicPlaying;
    case MODE_NOWPLAY:        // a little dashboard: keep it on
      return false;
    case MODE_RETRO:
      return !rcGifFrames;
    default:
      return true;
  }
}
void enterSleep() {
  modeBeforeSleep = currentMode;
  currentMode = MODE_SLEEP;
  if (ledcAttachChannel(TFT_BACKLIGHT, 5000, 8, BACKLIGHT_LEDC_CHANNEL)) ledcWrite(TFT_BACKLIGHT, 50); // dim
}
void wakeUp() {
  ledcDetach(TFT_BACKLIGHT);
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);
  currentMode = modeBeforeSleep;
  askNeedsDraw = true;
  if (currentMode == MODE_MENU) drawMenu();
  pycamera.speaker_tone(1320, 30);
}
void drawSleepScreen() {
  unsigned long now = millis();
  ui->fillScreen(CLAUDE_DARK);
  // Claude spark on top, slowly breathing
  drawClaudeLogo(ui, 120, 24, 14 + 2 * sinf(now / 900.0f), 0, CLAUDE_ORANGE);
  // Big clock: internet time, or the saved clock if we booted offline
  struct tm t;
  bool synced = localNow(&t);
  char hhmm[6] = "--:--";
  if (synced) snprintf(hhmm, sizeof(hhmm), "%d:%02d", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min);
  int len = strlen(hhmm);
  int x = (240 - len * 36 - (synced ? 28 : 0)) / 2;
  ui->setTextSize(6);
  ui->setCursor(x, 46);
  for (int i = 0; i < len; i++) { // the colon blinks every second
    bool hide = hhmm[i] == ':' && synced && t.tm_sec % 2;
    ui->setTextColor(hide ? CLAUDE_DARK : CLAUDE_CREAM);
    ui->print(hhmm[i]);
  }
  if (synced) {
    ui->setTextSize(2);
    ui->setTextColor(CLAUDE_ORANGE);
    ui->setCursor(x + len * 36 + 4, 78);
    ui->print(t.tm_hour < 12 ? "AM" : "PM");
    char date[16];
    strftime(date, sizeof(date), "%a, %d %b", &t);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(120 - strlen(date) * 6, 102);
    ui->print(date);
    if (timeApprox) { // restored from flash after an offline reboot
      ui->setTextSize(1);
      ui->setCursor(x - 10, 50);
      ui->print("~");
    }
  }
  // Sleeping bot in a nightcap, breathing slowly
  const int U = 10;
  int bx = 85, by = 150 + (int)(2 * sinf(now / 700.0f));
  drawBot(ui, bx, by, U, FACE_BLINK, 0, 0);
  ui->fillTriangle(bx + U, by, bx + 5 * U, by, bx + 6 * U, by - 2 * U, CLAUDE_BLUE);
  ui->fillCircle(bx + 6 * U, by - 2 * U, U / 2, CLAUDE_CREAM);
  // z z Z drifting up and growing
  for (int i = 0; i < 3; i++) {
    float p = ((now / 40 + i * 33) % 100) / 100.0f;
    ui->setTextSize(1 + (int)(p * 3));
    ui->setTextColor(p < 0.7f ? CLAUDE_CREAM : CLAUDE_GRAY);
    ui->setCursor(bx + 7 * U + 4 + (int)(p * 40), by - 16 - (int)(p * 24)); // drift right, clear of the date
    ui->print(p < 0.5f ? 'z' : 'Z');
  }
  // Quick peek at the phone link, then how to wake up
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  if (phoneOnline()) {
    char peek[44];
    if (usage.battery >= 0) snprintf(peek, sizeof(peek), "Phone %d%%   %lu Claude asks today", usage.battery, usage.requests);
    else snprintf(peek, sizeof(peek), "%lu Claude asks today", usage.requests);
    ui->setCursor(120 - strlen(peek) * 3, 210);
    ui->print(peek);
  }
  int mins;
  int next = nextAlarm(&mins);
  if (next >= 0) { // little bell + next alarm
    String a = clockText(alarms[next].hour, alarms[next].minute);
    int ax = 120 - (a.length() * 6 + 14) / 2;
    ui->fillCircle(ax + 4, 229, 4, CLAUDE_ORANGE);
    ui->fillRect(ax, 229, 9, 3, CLAUDE_ORANGE);
    ui->setTextColor(CLAUDE_ORANGE);
    ui->setCursor(ax + 14, 226);
    ui->print(a);
  } else {
    ui->setCursor(72, 226);
    ui->print("Press any button");
  }
  pushUi();
}
void sleepMode() {
  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 100) return;
  lastDraw = millis();
  drawSleepScreen();
}
// ===============================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  if (!pycamera.begin()) {
    Serial.println("Failed to initialize pyCamera interface");
    while (1) yield();
  }
  Serial.println("pyCamera hardware initialized!");
  if (!initUi()) {
    Serial.println("No PSRAM for the UI canvas - is PSRAM enabled in Tools?");
    while (1) yield();
  }
  prefs.begin("clawd", false);
  langSel = prefs.getUChar("lang", 1) % LANG_COUNT; // Hindi until you pick another
  triviaScore = prefs.getInt("trv_ok", 0);
  triviaAsked = prefs.getInt("trv_n", 0);
  if (!bridgeUrl.length()) bridgeUrl = prefs.getString("bridge", "");   // where the phone was last time
  if (!bridgeToken.length()) bridgeToken = prefs.getString("token", ""); // paired before
  calibrateAccel();
  loadAlarms();
  loadBestScores();
  playBootSplash();
  showBootQuote();
  unsigned long quoteShownAt = millis();
  // Status line under the bot while WiFi connects
  pycamera.setTextSize(1);
  pycamera.setTextColor(CLAUDE_CREAM, CLAUDE_DARK);
  pycamera.setCursor(60, 222);
  pycamera.print("Connecting to WiFi...");
  pycamera.initSD();
  pycamera.photoSize = FRAMESIZE_QVGA;
  if (!pycamera.initSD()) {
    Serial.println("SD init failed!");
    while (1);
  }
  // Allocate previous buffer for motion detection (240x240 RGB565)
  prev_buffer = (uint16_t*)ps_malloc(240 * 240 * sizeof(uint16_t));
  if (!prev_buffer) {
    Serial.println("Failed to allocate prev_buffer!");
    while (1);
  }
  WiFi.begin(ssid, password);
  client.setInsecure();
  // Internet time keeps syncing in the background whenever WiFi is up
  sntp_set_time_sync_notification_cb(onTimeSync);
  configTime(timezoneOffset, 0, "pool.ntp.org", "time.nist.gov");
  // Wait up to 15 s for WiFi, then carry on offline (it keeps retrying in loop())
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) { delay(250); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nReady!");
    unsigned long t0 = millis();
    while (!timeKnown && millis() - t0 < 5000) delay(50);
  } else {
    Serial.println("\nNo WiFi - carrying on offline");
  }
  if (!timeKnown) restoreClock(); // offline boot: continue from the last saved time
  loadEvents();                   // Telegram reminders survive reboots too
  loadPet();
  // Initialize display settings for the Gyro text
  pycamera.fb->setTextSize(2);
  // Setup LEDC timer
  ledc_timer_config_t timer_conf = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_8_BIT,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = 200000,
      .clk_cfg = LEDC_AUTO_CLK
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));
  // Initialize I2C as master
  Wire.begin(SDA_PIN, SCL_PIN);

  // Grab one camera frame so pycamera.fb has a buffer before any menu or game draws on it
  if (pycamera.captureFrame()) {
    esp_camera_fb_return(pycamera.frame);
    pycamera.frame = NULL;
  }
  // Bridge requests run on core 0 so the screen keeps animating while we wait
  xTaskCreatePinnedToCore(netTask, "net", 12288, NULL, 1, &netTaskHandle, 0); // HTTPS needs the bigger stack
  // The dance music has its own task so the beat stays steady while the screen redraws
  xTaskCreatePinnedToCore(danceTask, "dance", 4096, NULL, 2, &danceTaskHandle, 0);
  // Give the quote a few seconds on screen, then start on Clawd Talk, ready for a question
  while (millis() - quoteShownAt < 4000) delay(10);
  openTalk();
  lastActivity = millis();
}
void loop() {
  uint32_t buttons = pycamera.readButtons();
  // Any button change counts as activity; while asleep a press only wakes the device
  if (buttons != pycamera.last_button_state) {
    lastActivity = millis();
    if (currentMode == MODE_SLEEP) {
      wakeUp();
      return;
    }
  } else if (canSleepNow() && millis() - lastActivity > IDLE_AFTER_MS) {
    enterSleep();
  }
  serviceNetJob();
  keepTime();
  checkAlarms();
  pomoTick();
  petTick();
  talkTick();
  webTick();
  friendsTick();
  static unsigned long lastEventCheck = 0;
  if (millis() - lastEventCheck > 1000) { // Telegram reminders, in every mode
    lastEventCheck = millis();
    checkScheduledEvents();
  }
  // Keep track of where we are (phone GPS, else IP), for weather, planes and the ISS
  if (currentMode != MODE_ASK && currentMode != MODE_TALK && locationDue()) {
    locateTried = true;
    requestWidget(W_LOCATE);
  }
  // Check in with the phone every minute: usage, battery, and actions said into the phone app
  // (paused while Ask or Talk is waiting on Claude so questions aren't delayed, and in Dance)
  bool talkBusy = currentMode == MODE_TALK && talkState != TALK_IDLE && talkState != TALK_ANSWER;
  if (currentMode != MODE_ASK && !talkBusy && currentMode != MODE_DANCE && currentMode != MODE_ALARM_RING && !netBusy() &&
      (lastUsagePoll == 0 || millis() - lastUsagePoll > (phoneOnline() ? USAGE_POLL_MS : 10000UL))) { // look harder while lost
    requestUsage();
  }
  if (pycamera.justPressed(AWEXP_BUTTON_SEL)) {
    if (currentMode != MODE_MENU) {
      stopDance();
      leaveFunLab();
      currentMode = MODE_MENU;
      menuSelection = 0;
      drawMenu();
    }
  }
  switch (currentMode) {
    case MODE_MENU:
      handleMenuNavigation();
      break;
    case MODE_CAMERA:
      cameraMode();
      break;
    case MODE_GAME_MENU:
      gameMenuMode();
      break;
    case MODE_GAME:
      gameMode();
      break;
    case MODE_ALARMS:
      alarmsMode();
      break;
    case MODE_ALARM_RING:
      alarmRingMode();
      break;
    case MODE_FACE:
      faceMode();
      break;
    case MODE_WORLD:
      worldMode();
      break;
    case MODE_PET:
      petMode();
      break;
    case MODE_TOOLS:
      toolsMenuMode();
      break;
    case MODE_TOOL:
      toolMode();
      break;
    case MODE_HOME_ASSISTANT:
      homeAssistantMode();
      break;
    case MODE_USAGE:
      usageMode();
      break;
    case MODE_TALK:
      talkScreen();
      break;
    case MODE_ASK:
      askMode();
      break;
    case MODE_DANCE:
      danceMode();
      break;
    case MODE_SLEEP:
      sleepMode();
      break;
    case MODE_RETRO:
      retroMode();
      break;
    case MODE_NOWPLAY:
      nowPlayingMode();
      break;
    case MODE_BROWSER:
      browserMode();
      break;
    case MODE_MUSIC:
      musicMode();
      break;
    case MODE_FRIENDS:
      friendsMode();
      break;
    default:
      // Idle: perhaps live preview
      pycamera.captureFrame();
      pycamera.blitFrame();
      break;
  }
  delay(10);
}
// Draw main menu
void drawMenu() {
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print("Menu");
  const int VISIBLE = 6;
  int first = constrain(menuSelection - 2, 0, numMenuOptions - VISIBLE);
  if (first > 0) ui->fillTriangle(220, 14, 214, 22, 226, 22, CLAUDE_GRAY);
  if (first + VISIBLE < numMenuOptions) ui->fillTriangle(220, 222, 214, 214, 226, 214, CLAUDE_GRAY);
  for (int i = first; i < first + VISIBLE; i++) {
    int y = 40 + (i - first) * 30;
    bool selected = i == menuSelection;
    if (selected) ui->fillRoundRect(10, y, 220, 26, 8, CLAUDE_ORANGE);
    else ui->drawRoundRect(10, y, 220, 26, 8, CLAUDE_TRACK);
    ui->setTextColor(selected ? CLAUDE_DARK : CLAUDE_CREAM);
    ui->setCursor(22, y + 6);
    ui->print(menuOptions[i]);
  }
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(48, 226);
  ui->print("UP/DOWN move   OK open");
  pushUi();
}
// Handle navigation in menu
void handleMenuNavigation() {
  if (pycamera.justPressed(AWEXP_BUTTON_UP)) {
    menuSelection = (menuSelection - 1 + numMenuOptions) % numMenuOptions;
    drawMenu();
  }
  if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) {
    menuSelection = (menuSelection + 1) % numMenuOptions;
    drawMenu();
  }
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
    switch (menuSelection) {
      case 0: openTalk(); break;
      case 1: currentMode = MODE_ASK; break;
      case 2: currentMode = MODE_USAGE; break;
      case 3: currentMode = MODE_DANCE; startDance(0); petBoost(5); break;
      case 4: currentMode = MODE_PET; break;
      case 5: currentMode = MODE_ALARMS; alarmField = -1; break;
      case 6: currentMode = MODE_FACE; break;
      case 7: currentMode = MODE_WORLD; feedMode = false; break;
      case 8: currentMode = MODE_WORLD; feedMode = true; break;
      case 9: currentMode = MODE_RETRO; rcShare = false; break;
      case 10: currentMode = MODE_NOWPLAY; npPollAt = 0; break;
      case 11: currentMode = MODE_BROWSER; break;
      case 12: currentMode = MODE_MUSIC; musicScanned = false; break;
      case 13: currentMode = MODE_FRIENDS; chatWith = -1; break;
      case 14: currentMode = MODE_GAME_MENU; break;
      case 15: currentMode = MODE_TOOLS; break;
      case 16: currentMode = MODE_CAMERA; break;
      case 17: currentMode = MODE_HOME_ASSISTANT; break;
    }
  }
}
// Camera mode
void cameraMode() {
  pycamera.captureFrame();
  // Add UI if needed
  pycamera.fb->setCursor(10, 10);
  pycamera.fb->setTextSize(1);
  pycamera.fb->print("Camera Mode");
  pycamera.blitFrame();
  if (pycamera.justPressed(SHUTTER_BUTTON)) {
    pycamera.takePhoto("PHOTO", pycamera.photoSize);
    pycamera.speaker_tone(100, 50);
  }
  if (pycamera.justPressed(AWEXP_BUTTON_UP)) pycamera.photoSize = (framesize_t)((int)pycamera.photoSize + 1 % 11);
  if (pycamera.justPressed(AWEXP_BUTTON_DOWN)) pycamera.photoSize = (framesize_t)((int)pycamera.photoSize - 1 + 11 % 11);
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) pycamera.specialEffect = (pycamera.specialEffect + 6) % 7; pycamera.setSpecialEffect(pycamera.specialEffect);
  if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) pycamera.specialEffect = (pycamera.specialEffect + 1) % 7; pycamera.setSpecialEffect(pycamera.specialEffect);
}
// Home Assistant placeholder
void homeAssistantMode() {
  pycamera.captureFrame();
  // Motion detection only in security mode
  if (securityMode && prev_buffer) {
    uint16_t *curr = (uint16_t *)pycamera.fb->getBuffer();
    long total_diff = 0;
    long sum_x = 0;
    long sum_y = 0;
    int step = 2; // Subsample for efficiency (balance speed and accuracy)
    int pix_threshold = 20; // Lowered from 30 for better sensitivity
    for (int y = 0; y < 240; y += step) {
      for (int x = 0; x < 240; x += step) {
        uint16_t p1 = prev_buffer[y * 240 + x];
        uint16_t p2 = curr[y * 240 + x];
        int r1 = (p1 >> 11) & 0x1F;
        int g1 = (p1 >> 5) & 0x3F;
        int b1 = p1 & 0x1F;
        int r2 = (p2 >> 11) & 0x1F;
        int g2 = (p2 >> 5) & 0x3F;
        int b2 = p2 & 0x1F;
        int pix_diff = abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2);
        if (pix_diff > pix_threshold) {
          sum_x += (long)x * pix_diff;
          sum_y += (long)y * pix_diff;
          total_diff += pix_diff;
        }
      }
    }
    int num_pixels = (240 / step) * (240 / step);
    float avg_diff = (float)total_diff / (num_pixels * 3.0 * 31.0); // Normalized (adjust threshold as needed)
    Serial.printf("Average diff: %.2f\n", avg_diff); // Added for debugging
    if (total_diff > 0) {
      int cx = sum_x / total_diff;
      int cy = sum_y / total_diff;
      Serial.printf("Motion blob center at (%d, %d)\n", cx, cy);
    }
    if (avg_diff > 0.05 && (millis() - last_snap_time > SNAP_COOLDOWN)) { // Lowered threshold from 0.2 for better sensitivity
      Serial.println("High motion detected! Taking snapshot...");
      sendI2CCommandTo(ARDUINO_ADDR, "High motion detected! Taking snapshot...");
      takeAndSendPhoto(0);
      last_snap_time = millis();
    }
  }
  memcpy(prev_buffer, pycamera.fb->getBuffer(), 240 * 240 * sizeof(uint16_t));
  // --- LIVE GYRO DATA DISPLAY ---
  float x, y, z;
  if (pycamera.readAccelData(&x, &y, &z)) {
    // Positioning text at the top of the frame
    Serial.printf("ACCEL: X=%0.2f, Y=%0.2f, Z=%0.2f\n", x, y, z);
    pycamera.fb->setCursor(10, 10);
    pycamera.fb->setTextColor(0x780F); // Your favorite Purple color!
    pycamera.fb->printf("X:%0.1f Y:%0.1f Z:%0.1f", x, y, z);
  }
  // Green object tracking if enabled
  if (tracking) {
    trackGreenObject();
  }
  // Trigger snapshot with the Shutter button
  if (pycamera.justPressed(SHUTTER_BUTTON)) {
    Serial.println("Taking Snapshot...");
    takeAndSendPhoto(0);
  }
  // Check for serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "snap") {
      Serial.println("Serial snap command received!");
      takeAndSendPhoto(0);
    }
  }
  // Poll Telegram for new messages
  if (millis() - bot_lasttime > BOT_MTBS) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    handleNewMessages(numNewMessages);
    bot_lasttime = millis();
  }
  pycamera.blitFrame();
  if (pycamera.justPressed(AWEXP_BUTTON_SEL)) currentMode = MODE_MENU;
}
void sendI2CCommandTo(uint8_t addr, const char* cmd) {
  Wire.beginTransmission(addr);
  Wire.write((const uint8_t*)cmd, strlen(cmd));
  Wire.endTransmission();
}
// Helper function to send Telegram messages
// Centralizes bot.sendMessage calls for easy parameter modification
void sendTelegramMessage(String chat_id, String message, int reply_to_message_id) {
  bot.sendMessage(chat_id, message, "");
}
// Telegram reminders are kept in flash so a reboot doesn't lose them
void saveEvents() {
  JsonDocument doc;
  for (auto &ev : scheduledEvents) {
    JsonObject o = doc.add<JsonObject>();
    o["t"] = (int64_t)ev.eventTime;
    o["d"] = ev.details;
    o["c"] = ev.arduinoCmd;
    o["r"] = ev.reminder;
  }
  String out;
  serializeJson(doc, out);
  prefs.putString("events", out);
}
void loadEvents() {
  JsonDocument doc;
  if (deserializeJson(doc, prefs.getString("events", "[]"))) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    Event ev;
    ev.eventTime = (time_t)(o["t"] | (int64_t)0);
    ev.details = (const char *)(o["d"] | "");
    ev.arduinoCmd = (const char *)(o["c"] | "none");
    ev.reminder = (const char *)(o["r"] | "");
    scheduledEvents.push_back(ev);
  }
}
void checkScheduledEvents() {
  if (!timeKnown) return; // no clock yet: don't fire everything at once
  time_t now = time(nullptr);
  for (auto it = scheduledEvents.begin(); it != scheduledEvents.end(); ) {
    if (now >= it->eventTime && !it->triggered) {
      if (it->arduinoCmd != "none") {
        sendI2CCommandTo(ARDUINO_ADDR, it->arduinoCmd.c_str());
        Serial.printf("Sent command '%s' to Arduino\n", it->arduinoCmd.c_str());
      }
      if (!it->reminder.isEmpty()) {
        sendTelegramMessage(CHAT_ID, it->reminder);
      }
      it->triggered = true;
      it = scheduledEvents.erase(it); // Remove after trigger
      saveEvents();
    } else {
      ++it;
    }
  }
}
bool parseSchedule(String text, String chat_id) {
  // Split by lines
  String lines[5];
  int idx = 0;
  int pos = 0;
  while (idx < 5) {
    int nl = text.indexOf('\n', pos);
    if (nl == -1) {
      lines[idx] = text.substring(pos);
      break;
    }
    lines[idx] = text.substring(pos, nl);
    pos = nl + 1;
    idx++;
  }
  String timeStr, dateStr, details, arduinoCmd, reminder;
  for (int i = 0; i <= idx; i++) {
    String line = lines[i];
    line.trim();
    if (line.startsWith("Time:")) {
      timeStr = line.substring(5);
      timeStr.trim();
    } else if (line.startsWith("Date:")) {
      dateStr = line.substring(5);
      dateStr.trim();
    } else if (line.startsWith("Details:")) {
      details = line.substring(8);
      details.trim();
    } else if (line.startsWith("Command:")) {
      arduinoCmd = line.substring(8);
      arduinoCmd.trim();
    } else if (line.startsWith("Reminder:")) {
      reminder = line.substring(9);
      reminder.trim();
    }
  }
  if (timeStr.isEmpty() || dateStr.isEmpty()) {
    return false;
  }
  // Parse date and time - support both MM/DD/YYYY and DD/MM/YYYY
  struct tm tm = {0};
  int day, month, year;
  if (sscanf(dateStr.c_str(), "%d/%d/%d", &month, &day, &year) != 3) {
    return false;
  }
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  int hour, min;
  char ampm[3];
  if (sscanf(timeStr.c_str(), "%d:%d %2s", &hour, &min, ampm) != 3) {
    return false;
  }
  if (strcasecmp(ampm, "pm") == 0 && hour < 12) hour += 12;
  else if (strcasecmp(ampm, "am") == 0 && hour == 12) hour = 0;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_sec = 0;
  time_t eventTime = mktime(&tm);
  Event ev;
  ev.eventTime = eventTime;
  ev.details = details;
  ev.arduinoCmd = arduinoCmd;
  ev.reminder = reminder;
  scheduledEvents.push_back(ev);
  saveEvents();
  return true;
}
void trackGreenObject() {
  uint16_t *fb = (uint16_t *)pycamera.fb->getBuffer();
  int minx = 239, maxx = 0, miny = 239, maxy = 0;
  bool found = false;
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 240; x++) {
      uint16_t pix = fb[y * 240 + x];
      uint8_t r = ((pix >> 11) & 0x1F) << 3;
      uint8_t g = ((pix >> 5) & 0x3F) << 2;
      uint8_t b = (pix & 0x1F) << 3;
      // Green detection threshold (adjust as needed)
      if (g > 100 && g > r + 50 && g > b + 50) {
        found = true;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
      }
    }
  }
  if (found && (maxx - minx > 10) && (maxy - miny > 10)) {
    int cx = (minx + maxx) / 2;
    int cy = (miny + maxy) / 2;
    // Draw bounding box (red)
    pycamera.fb->drawRect(minx, miny, maxx - minx + 1, maxy - miny + 1, 0xF800);
    // Send position to Arduino via I2C
    Wire.beginTransmission(ARDUINO_ADDR);
    Wire.write((uint8_t)cx);
    Wire.write((uint8_t)cy);
    Wire.endTransmission();
    Serial.printf("Green object at (%d, %d)\n", cx, cy);
  }
}
void sendDanceCommand() {
  sendI2CCommandTo(ARDUINO_ADDR, "dance");
  Serial.println("Sent 'dance' command to Arduino");
}
void takeAndSendPhoto(int reply_to_message_id) {
  pycamera.setNeopixel(0x800080); // Purple flash
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    if (reply_to_message_id != 0) {
      sendTelegramMessage(CHAT_ID, "❌ Camera capture failed!", reply_to_message_id);
    }
    return;
  }
  fb_length = fb->len;
  fb_buffer = fb->buf;
  currentByte = 0;
  Serial.println("Sending to Telegram...");
  // Note: UniversalTelegramBot's sendPhotoByBinary doesn't support reply_to_message_id directly
  // The photo itself won't be a reply, but we send a confirmation message as a reply
  if (bot.sendPhotoByBinary(CHAT_ID, "image/jpeg", fb->len,
                            isMoreDataAvailable, photoNextByte,
                            nullptr, nullptr)) {
    Serial.println("Photo sent successfully!");
    if (reply_to_message_id != 0) {
      sendTelegramMessage(CHAT_ID, "✅ Photo captured and sent!", reply_to_message_id);
    }
  } else {
    Serial.println("Telegram upload failed.");
    if (reply_to_message_id != 0) {
      sendTelegramMessage(CHAT_ID, "❌ Failed to send photo!", reply_to_message_id);
    }
  }
  esp_camera_fb_return(fb);
  pycamera.setNeopixel(0x000000);
}
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    int message_id = bot.messages[i].message_id; // Extract message_id for reply tracking
  
    // Process messages from authorized user OR messages starting with "app_" prefix
    if (chat_id != CHAT_ID && !text.startsWith("app_")) continue;
    // Remove "app_" prefix if present
    if (text.startsWith("app_")) {
      text = text.substring(4); // Remove "app_" (4 characters)
    }
   // text.toLowerCase(); // Make case-insensitive
    // Command handling with reply tracking
    if (text == "send me a pic" || text == "/pic" || text == "pic") {
      Serial.println("Telegram pic command received!");
      takeAndSendPhoto(message_id);
    } else if (text == "/record" || text == "record") {
      Serial.println("Telegram record command received!");
      sendTelegramMessage(chat_id, "🎤 Recording audio...", message_id);
      recordAudio();
    } else if (text == "play" || text == "letsplay" || text == "/letsplay") {
      Serial.println("Telegram play command received!");
      sendTelegramMessage(chat_id, "🔊 Playing audio...", message_id);
      playAudio();
    } else if (text == "/letsplay" || text == "letsplay") {
      tracking = true;
      sendTelegramMessage(chat_id, "🎮 Tracking started! Let's play!", message_id);
      Serial.println("Tracking enabled!");
    } else if (text == "/stopdance" || text == "stopdance") {
      tracking = false;
      sendTelegramMessage(chat_id, "🛑 Dance stopped!", message_id);
      sendI2CCommandTo(ARDUINO_ADDR, "stopdance");
      sendI2CCommandTo(ARDUINO2_ADDR, "stop");
      Serial.println("stopdance");
    } else if (text == "stop") {
      tracking = false;
      securityMode = false;
      sendTelegramMessage(chat_id, "🛑 All commands stopped!", message_id);
      sendI2CCommandTo(ARDUINO_ADDR, "stop");
      sendI2CCommandTo(ARDUINO2_ADDR, "stop");
      sendI2CCommandTo(ARDUINO2_ADDR, "wake");
      Serial.println("stop");
    } else if (text == "dance" || text == "/dance") {
      sendDanceCommand();
      sendTelegramMessage(chat_id, "💃 Dance mode activated!", message_id);
      sendI2CCommandTo(ARDUINO2_ADDR, "exp1");
      sendI2CCommandTo(ARDUINO_ADDR, "dance");
    } else if (text == "/securitymode" || text == "securitymode") {
      securityMode = true;
      sendTelegramMessage(chat_id, "🔒 Security mode enabled! Monitoring for motion...", message_id);
      sendI2CCommandTo(ARDUINO_ADDR, "Secure");
      sendI2CCommandTo(ARDUINO2_ADDR, "secure");
      Serial.println("Security mode enabled!");
    } else if (text == "/iamhome" || text == "iamhome") {
      securityMode = false;
      sendTelegramMessage(chat_id, "🏠 Welcome home! Security mode disabled.", message_id);
      sendI2CCommandTo(ARDUINO_ADDR, "Security mode disabled!");
      Serial.println("Security mode disabled!");
      sendI2CCommandTo(ARDUINO2_ADDR, "wake");
      sendI2CCommandTo(ARDUINO_ADDR, "wakeup");
      sendI2CCommandTo(ARDUINO_ADDR, "stop");
    } else if (text == "lighton") {
      sendControlCommand("light_on");
      sendTelegramMessage(chat_id, "💡 Light turned on!", message_id);
      Serial.println("Light on command sent");
    } else if (text == "lightoff") {
      sendControlCommand("light_off");
      sendTelegramMessage(chat_id, "🌙 Light turned off!", message_id);
      Serial.println("Light off command sent");
    } else if (text == "fanon") {
      sendControlCommand("fan_on");
      sendTelegramMessage(chat_id, "🪭 Fan turned on!", message_id);
      Serial.println("Fan on command sent");
    } else if (text == "fanoff") {
      sendControlCommand("fan_off");
      sendTelegramMessage(chat_id, "✋ Fan turned off!", message_id);
      Serial.println("Fan off command sent");
    } else if (text == "ping" || text == "hi") {
      sendTelegramMessage(chat_id, "pong", message_id);
    } else if (text == "schedule" || text == "/schedule") {
      sendTelegramMessage(chat_id, "📅 Please schedule an event in this format:\nTime: 12:00 pm\nDate: 2/3/2026\nDetails: \"i have a meeting to attend\"\nCommand: \"medicine_slot_08\" or \"none\"\nReminder: \"reminder text\"", message_id);
      awaitingSchedule = true;
    } else if (text.startsWith("/instantschedule ") || text.startsWith("instantschedule ")) {
      String scheduleData = text.substring(17); // Extract text after "/instantschedule "
      scheduleData.trim();
      if (scheduleData.length() > 0) {
        if (parseSchedule(scheduleData, chat_id)) {
          sendTelegramMessage(chat_id, "✅ Event scheduled successfully!", message_id);
        } else {
          sendTelegramMessage(chat_id, "❌ Failed to schedule event. Check format.", message_id);
        }
      } else {
        sendTelegramMessage(chat_id, "❌ Please provide schedule details after the command.", message_id);
      }
    } else if (text == "sleep") {
      sendI2CCommandTo(ARDUINO2_ADDR, "sleep");
      sendI2CCommandTo(ARDUINO_ADDR, "sleep");
      sendTelegramMessage(chat_id, "😴 Sleep mode enabled", message_id);
    } else if (text == "wakeup") {
      sendI2CCommandTo(ARDUINO2_ADDR, "wake");
      sendTelegramMessage(chat_id, "👋 Waking up the robot!", message_id);
      sendI2CCommandTo(ARDUINO_ADDR, "wakeup");
    } else if (text == "exp1") {
      sendI2CCommandTo(ARDUINO2_ADDR, "exp1");
      sendTelegramMessage(chat_id, " Expression changed ", message_id);
    } else if (text == "exp2") {
      sendI2CCommandTo(ARDUINO2_ADDR, "exp2");
      sendTelegramMessage(chat_id, "😊 Expression changed to normal", message_id);
    } else if (awaitingSchedule) {
      if (parseSchedule(text, chat_id)) {
        sendTelegramMessage(chat_id, "✅ Event scheduled successfully!", message_id);
        awaitingSchedule = false;
      } else {
        sendTelegramMessage(chat_id, "❌ Failed to parse schedule. Please try again.", message_id);
      }
    }
  }
}
void recordAudio() {
  const int sampleRate = 8000;
  const int duration = 5;
  int numSamples = sampleRate * duration;
  uint16_t *samples = (uint16_t*)malloc(numSamples * sizeof(uint16_t));
  if (!samples) {
    Serial.println("Failed to allocate samples!");
    return;
  }
  unsigned long last = micros();
  for (int i = 0; i < numSamples; i++) {
    while (micros() - last < (1000000UL / sampleRate));
    last += (1000000UL / sampleRate);
    samples[i] = analogRead(MIC);
  }
  File file = pycamera.sd.open("last_voice.raw", FILE_WRITE);
  if (file) {
    file.write((uint8_t*)samples, numSamples * sizeof(uint16_t));
    file.close();
    Serial.println("Audio recorded and saved!");
  } else {
    Serial.println("Failed to open file for writing!");
  }
  free(samples);
}
void playAudio() {
  const int sampleRate = 8000;
  File file = pycamera.sd.open("last_voice.raw", FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading!");
    return;
  }
  size_t size = file.size();
  if (size == 0) {
    file.close();
    return;
  }
  uint16_t *samples = (uint16_t*)malloc(size);
  if (!samples) {
    Serial.println("Failed to allocate samples!");
    file.close();
    return;
  }
  file.read((uint8_t*)samples, size);
  file.close();
  int numSamples = size / sizeof(uint16_t);
  // Configure LEDC channel
  ledc_channel_config_t chan_conf = {
      .gpio_num = SPEAKER,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
      .hpoint = 0,
      .flags = { .output_invert = 0 }
  };
  ESP_ERROR_CHECK(ledc_channel_config(&chan_conf));
  // Unmute speaker
  pycamera.aw.digitalWrite(AWEXP_SPKR_SD, HIGH);
  unsigned long last = micros();
  for (int i = 0; i < numSamples; i++) {
    while (micros() - last < (1000000UL / sampleRate));
    last += (1000000UL / sampleRate);
    uint32_t duty = samples[i] >> 4; // Scale 12-bit to 8-bit
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  }
  // Stop and mute
  ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
  pycamera.aw.digitalWrite(AWEXP_SPKR_SD, LOW);
  // Reset pin
  gpio_reset_pin((gpio_num_t)SPEAKER);
  free(samples);
  Serial.println("Audio playback complete!");
}
// New function to send HTTP requests to the control ESP32
void sendControlCommand(String endpoint) {
  WiFiClient httpClient;
  if (httpClient.connect(controllerIP, 80)) {
    httpClient.print(String("GET /") + endpoint + " HTTP/1.1\r\n" +
                     "Host: " + controllerIP + "\r\n" +
                     "Connection: close\r\n\r\n");
    unsigned long timeout = millis();
    while (httpClient.connected() && millis() - timeout < 5000) {
      if (httpClient.available()) {
        String line = httpClient.readStringUntil('\n');
        // You can process the response if needed, but for now, we just send the request
      }
    }
    httpClient.stop();
    Serial.println("Command sent to control ESP32: " + endpoint);
  } else {
    Serial.println("Failed to connect to control ESP32");
  }
}