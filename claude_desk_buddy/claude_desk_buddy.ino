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
// Claude screens (declared up here so Arduino's auto-generated prototypes can see them)
enum Face { FACE_NORMAL, FACE_BLINK, FACE_HAPPY, FACE_WORRIED, FACE_DEAD, FACE_LOOK_LEFT, FACE_LOOK_RIGHT };
enum NetJob { JOB_NONE, JOB_USAGE, JOB_ASK_IMAGE, JOB_FACE, JOB_WEB, JOB_QR };
enum AskState { ASK_PREVIEW, ASK_THINKING, ASK_ANSWER };
struct Alarm { bool on; uint8_t hour, minute, repeat; };
// --- CREDENTIALS --- (WiFi + Telegram live in secrets.h, which is not committed)
#include "secrets.h"
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
// PC running claude_bridge.py (it prints the exact line to paste here when it starts).
// If the PC's IP changes, the board finds the bridge again by itself.
#define BRIDGE_URL "http://10.66.196.249:8765"
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
  MODE_TOOL
};
Mode currentMode = MODE_IDLE;
int menuSelection = 0;
const char* menuOptions[] = {"Claude Usage", "Claude Ask", "Claude Dance", "Clawd Pet", "Alarms", "Face Finder",
                             "World Live", "Games", "Toolbox", "Camera Mode", "Home Assistant"};
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
TaskHandle_t netTaskHandle = NULL;
String bridgeUrl = BRIDGE_URL; // replaced automatically if the bridge turns up at another IP
// Broadcasts "where are you?" on the LAN; claude_bridge.py answers from the PC's current IP
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
        bridgeUrl = "http://" + udp.remoteIP().toString() + ":" + String(port);
        Serial.println("Found bridge at " + bridgeUrl);
        found = true;
      }
    }
    delay(20);
  }
  udp.stop();
  return found;
}
int bridgeRequest() {
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(netJob == JOB_ASK_IMAGE ? 60000 : 10000); // Claude can take a while
  int status = -1;
  if (http.begin(bridgeUrl + netPath)) {
    if (netBody) {
      http.addHeader("Content-Type", netContentType);
      status = http.POST(netBody, netBodyLen);
    } else {
      status = http.GET();
    }
    if (status > 0) netResponse = http.getString();
    http.end();
  }
  return status;
}
// Plain HTTPS GET to a public web API (netPath holds the full URL)
int webRequest() {
  WiFiClientSecure tls;
  tls.setInsecure(); // these are public, read-only feeds
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  http.setUserAgent("ClaudeDeskBuddy/1.0");
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
      netStatus = bridgeRequest();
      if (netStatus < 0 && discoverBridge()) netStatus = bridgeRequest(); // PC moved? retry once
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

// ===================== CLAUDE USAGE =====================
struct UsageInfo {
  bool valid = false;
  float sessionPct = 0, weeklyPct = 0;
  String sessionReset, weeklyReset, plan, error;
  int rows = 0;
  String rowName[4];
  int rowPct[4];
  unsigned long fetchedAt = 0;
} usage;
float shownSession = 0, shownWeekly = 0; // animated bar values chasing the real ones
int usagePage = 0;                       // 0 = session/weekly, 1 = weekly breakdown by app
unsigned long lastUsagePoll = 0;
const unsigned long USAGE_POLL_MS = 60000;
int alertLevel = 0;                      // 1 = warned at 80%, 2 = warned at 100%
unsigned long waveUntil = 0;
void requestUsage(bool fresh) {
  lastUsagePoll = millis();
  startNetJob(JOB_USAGE, fresh ? "/usage?fresh=1" : "/usage", NULL, 0, "");
}
void handleUsageResult(bool parsed, JsonDocument &doc) {
  if (!parsed) {
    usage.error = netStatus < 0 ? "No bridge at " + bridgeUrl.substring(7) + " - is claude_bridge.py running?"
                                : "Bridge error " + String(netStatus);
    return;
  }
  if (!(doc["ok"] | false)) {
    usage.error = (const char *)(doc["error"] | "Unknown error");
    return;
  }
  float oldSession = usage.sessionPct;
  usage.valid = true;
  usage.error = "";
  usage.plan = (const char *)(doc["plan"] | "");
  usage.sessionPct = doc["session"]["pct"] | 0.0f;
  usage.sessionReset = (const char *)(doc["session"]["resets"] | "");
  usage.weeklyPct = doc["weekly"]["pct"] | 0.0f;
  usage.weeklyReset = (const char *)(doc["weekly"]["resets"] | "");
  usage.rows = 0;
  for (JsonObject row : doc["breakdown"].as<JsonArray>()) {
    if (usage.rows >= 4) break;
    usage.rowName[usage.rows] = (const char *)(row["name"] | "");
    usage.rowPct[usage.rows] = row["pct"] | 0;
    usage.rows++;
  }
  usage.fetchedAt = millis();
  if (fabsf(usage.sessionPct - oldSession) >= 1.0f) waveUntil = millis() + 1500;
  // Beep once when the session crosses 80%, and again at 100%
  int level = usage.sessionPct >= 100 ? 2 : usage.sessionPct >= 80 ? 1 : 0;
  if (level > alertLevel && !danceOn) { // the dance owns the speaker
    for (int i = 0; i <= level; i++) {
      pycamera.speaker_tone(1500, 80);
      delay(60);
    }
  }
  alertLevel = level;
}
uint16_t levelColor(float pct) {
  return pct >= 90 ? CLAUDE_RED : pct >= 70 ? CLAUDE_AMBER : CLAUDE_ORANGE;
}
const char *usageQuip(float pct) {
  if (pct >= 100) return "Limit reached. Coffee break?";
  if (pct >= 80) return "Running low! Save it for the big stuff.";
  if (pct >= 50) return "Past halfway. Pace yourself.";
  return "Plenty of room. Let's build something!";
}
void drawUsageHeader(const char *title) {
  ui->fillScreen(CLAUDE_DARK);
  drawClaudeLogo(ui, 20, 18, 11, 0, CLAUDE_ORANGE);
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(38, 11);
  ui->print(title);
  if (usage.plan.length()) {
    int w = usage.plan.length() * 6 + 8;
    ui->drawRoundRect(232 - w, 11, w, 15, 4, CLAUDE_ORANGE);
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_ORANGE);
    ui->setCursor(236 - w, 15);
    ui->print(usage.plan);
  }
}
void drawUsageFooter(unsigned long now) {
  bool online = usage.valid && usage.error.length() == 0;
  ui->fillCircle(10, 229, 3, online ? CLAUDE_GREEN : CLAUDE_RED);
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
// Label, big percentage, rounded bar and "Resets in ..." line, starting at y
void drawUsageBar(int y, const char *label, const char *span, float pct, const String &reset) {
  ui->setTextSize(2);
  ui->setTextColor(CLAUDE_CREAM);
  ui->setCursor(12, y);
  ui->print(label);
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  ui->setCursor(ui->getCursorX() + 4, y + 6);
  ui->print(span);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", (int)(pct + 0.5f));
  ui->setTextSize(2);
  ui->setTextColor(levelColor(pct));
  ui->setCursor(228 - strlen(buf) * 12, y);
  ui->print(buf);
  ui->fillRoundRect(12, y + 22, 216, 12, 6, CLAUDE_TRACK);
  int fw = constrain((int)(216 * pct / 100.0f), 0, 216);
  if (fw > 0) ui->fillRoundRect(12, y + 22, max(fw, 12), 12, 6, levelColor(pct));
  if (reset.length()) {
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(12, y + 40);
    ui->print("Resets in " + reset);
  }
}
void drawUsageMain(unsigned long now) {
  drawUsageHeader("Claude Usage");
  drawUsageBar(42, "Session", "(5hr)", shownSession, usage.sessionReset);
  drawUsageBar(102, "Weekly", "(7 day)", shownWeekly, usage.weeklyReset);
  // Mood bot: its face and quip follow whichever limit is closer to running out
  float worst = max(usage.sessionPct, usage.weeklyPct);
  Face face = !usage.valid || usage.error.length() ? FACE_WORRIED
              : worst >= 100 ? FACE_DEAD
              : worst >= 80  ? FACE_WORRIED
              : worst >= 50  ? FACE_NORMAL
                             : FACE_HAPPY;
  if (face != FACE_DEAD && now % 4000 < 150) face = FACE_BLINK;
  int arm = now < waveUntil ? ((now / 180) % 2 ? 2 : 1) : 0;
  drawBot(ui, 20, 180, 6, face, arm, 0);
  drawBubble(ui, 76, 164, 156, 46, 66, 192);
  String quip = usage.error.length() ? usage.error : usage.valid ? String(usageQuip(worst)) : String("Fetching your usage...");
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
void drawUsageBreakdown(unsigned long now) {
  drawUsageHeader("This week");
  if (usage.rows == 0) {
    ui->setTextSize(1);
    ui->setTextColor(CLAUDE_GRAY);
    ui->setCursor(12, 60);
    ui->print("No breakdown yet");
  }
  for (int i = 0; i < usage.rows; i++) {
    int y = 46 + i * 42;
    ui->setTextSize(2);
    ui->setTextColor(CLAUDE_CREAM);
    ui->setCursor(12, y);
    ui->print(usage.rowName[i]);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", usage.rowPct[i]);
    ui->setTextColor(CLAUDE_ORANGE);
    ui->setCursor(228 - strlen(buf) * 12, y);
    ui->print(buf);
    ui->fillRoundRect(12, y + 20, 216, 8, 4, CLAUDE_TRACK);
    int fw = 216 * constrain(usage.rowPct[i], 0, 100) / 100;
    if (fw > 0) ui->fillRoundRect(12, y + 20, max(fw, 8), 8, 4, CLAUDE_ORANGE);
  }
  drawUsageFooter(now);
}
// Animate a shown value toward its target; returns true while still moving
bool chase(float &shown, float target) {
  if (fabsf(target - shown) < 0.3f) {
    shown = target;
    return false;
  }
  shown += (target - shown) * 0.15f;
  return true;
}
void usageMode() {
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) {
    requestUsage(true);
    pycamera.speaker_tone(1200, 20);
  }
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT) || pycamera.justPressed(AWEXP_BUTTON_RIGHT)) usagePage ^= 1;
  if (pycamera.justPressed(SHUTTER_BUTTON)) playBotHi(); // say hi again
  static unsigned long lastDraw = 0;
  unsigned long now = millis();
  if (now - lastDraw < 66) return; // ~15 fps is plenty
  lastDraw = now;
  chase(shownSession, usage.sessionPct);
  chase(shownWeekly, usage.weeklyPct);
  if (usagePage == 0) drawUsageMain(now);
  else drawUsageBreakdown(now);
  pushUi();
}

// ===================== CLAUDE ASK =====================
// Point the camera at something and press Shutter (or OK): Claude says what it is, or solves it
#define ASK_MAX_LINES 30
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
    String msg = netStatus < 0 ? String("Can't reach the bridge on your PC.") : "Bridge error " + String(netStatus);
    showAskResult(msg, true);
    return;
  }
  showAskResult((const char *)(doc["answer"] | ""), !(doc["ok"] | false));
}
void handleFaceResult(bool parsed, JsonDocument &doc);
void handleWebResult(int status, const String &body);
void handleQrResult(bool parsed, JsonDocument &doc);
// Called every loop(): hands a finished network job's result to its screen
void serviceNetJob() {
  if (!netDone) return;
  NetJob job = netJob;
  netDone = false;
  if (netBody) {
    free(netBody);
    netBody = NULL;
  }
  if (job == JOB_WEB) {
    handleWebResult(netStatus, netResponse);
  } else {
    JsonDocument doc;
    bool parsed = netStatus == 200 && !deserializeJson(doc, netResponse);
    if (job == JOB_USAGE) handleUsageResult(parsed, doc);
    else if (job == JOB_FACE) handleFaceResult(parsed, doc);
    else if (job == JOB_QR) handleQrResult(parsed, doc);
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
  if (!waitNetIdle(8000) || !startNetJob(JOB_ASK_IMAGE, "/ask/image", jpg, len, "image/jpeg")) {
    free(jpg);
    showAskResult("No connection to the bridge.", true);
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
  fb->fillRect(0, 218, 240, 22, CLAUDE_DARK);
  fb->setCursor(18, 226);
  fb->print("[Shutter] ask Claude about this");
  pycamera.blitFrame();
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
  ui->print(askFailed ? "Something went wrong" : "About your photo");
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
Preferences prefs; // flash storage: alarms, last known time, high scores, reminders
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
// The camera streams its small preview frames to claude_bridge.py, which finds faces with
// OpenCV. (This board's ESP32 core no longer ships Espressif's on-chip face detector.)
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
  if (faceBridgeDown) fb->print("Face Finder - bridge offline");
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

// ===================== WORLD LIVE =====================
// Free web data the board fetches itself over WiFi, no API keys: Open-Meteo (weather),
// OpenSky (planes), wheretheiss.at (ISS), USGS (earthquakes) and CoinGecko (crypto).
// Your location is guessed from your internet connection (ipwho.is) unless you pin it here.
#define MY_LAT 0.0f // e.g. 23.2599 to pin your location; 0 = automatic
#define MY_LON 0.0f
#define W_WEATHER 0
#define W_PLANES 1
#define W_ISS 2
#define W_QUAKES 3
#define W_CRYPTO 4
#define W_PAGES 5
#define W_LOCATE 5 // not a page: the one-off location lookup
#define CLAUDE_LAND 0x3A67  // #384E3C
#define CLAUDE_CLOUD 0xCE59 // #C8C8CC
const char *W_TITLES[] = {"Weather", "Planes nearby", "ISS tracker", "Earthquakes", "Crypto"};
const unsigned long W_REFRESH_MS[] = {900000, 60000, 10000, 600000, 120000, 0};
unsigned long wFetchedAt[W_PAGES + 1];
bool wHasData[W_PAGES + 1];
String wError[W_PAGES + 1];
int worldPage = 0;
int webWidget = -1; // which widget the running web request is for
float myLat = MY_LAT, myLon = MY_LON;
String myCity;
bool locationKnown = MY_LAT != 0.0f || MY_LON != 0.0f;
bool locateTried = false;
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
    case W_LOCATE:
      return "https://ipwho.is/?fields=success,city,latitude,longitude";
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
    default:
      return "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum,solana,dogecoin&vs_currencies=inr&include_24hr_change=true";
  }
}
void requestWidget(int w) {
  if (startNetJob(JOB_WEB, widgetUrl(w), NULL, 0, "")) {
    webWidget = w;
    wFetchedAt[w] = millis();
  }
}
// Called while World Live is on screen: fetch the visible page when it's due
void worldTick() {
  if (netBusy()) return;
  int w = worldPage;
  if ((w == W_WEATHER || w == W_PLANES) && !locationKnown) {
    if (!locateTried) {
      locateTried = true;
      requestWidget(W_LOCATE);
    }
    return;
  }
  if (!wFetchedAt[w] || millis() - wFetchedAt[w] > W_REFRESH_MS[w]) requestWidget(w);
}
void handleWebResult(int status, const String &body) {
  int w = webWidget;
  webWidget = -1;
  if (w < 0) return;
  if (status != 200) {
    wError[w] = status == 429 ? String("Too many requests - try later")
              : status < 0    ? String("No internet connection")
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
        myLat = doc["latitude"] | 0.0f;
        myLon = doc["longitude"] | 0.0f;
        myCity = (const char *)(doc["city"] | "");
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
void worldMode() {
  if (pycamera.justPressed(AWEXP_BUTTON_LEFT)) worldPage = (worldPage + W_PAGES - 1) % W_PAGES;
  if (pycamera.justPressed(AWEXP_BUTTON_RIGHT)) worldPage = (worldPage + 1) % W_PAGES;
  if (pycamera.justPressed(AWEXP_BUTTON_OK)) wFetchedAt[worldPage] = 0; // refresh now
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
  bool loading = webWidget == w || (webWidget == W_LOCATE && (w == W_WEATHER || w == W_PLANES));
  ui->fillCircle(230, 11, 4, loading ? CLAUDE_AMBER : wError[w].length() ? CLAUDE_RED : wHasData[w] ? CLAUDE_GREEN : CLAUDE_TRACK);
  if (!wHasData[w]) {
    String msg = wError[w].length()                                         ? wError[w]
                 : (w == W_WEATHER || w == W_PLANES) && !locationKnown      ? String(locateTried && !netBusy() ? "Couldn't find your location" : "Finding your location...")
                 : WiFi.status() != WL_CONNECTED                            ? String("Waiting for WiFi...")
                                                                            : String("Loading...");
    drawCentered(msg, 110, 1, CLAUDE_GRAY);
  } else {
    switch (w) {
      case W_WEATHER: drawWeather(); break;
      case W_PLANES: drawPlanes(); break;
      case W_ISS: drawIss(); break;
      case W_QUAKES: drawQuakes(); break;
      case W_CRYPTO: drawCrypto(); break;
    }
  }
  for (int i = 0; i < W_PAGES; i++) ui->fillCircle(96 + i * 12, 233, i == w ? 3 : 2, i == w ? CLAUDE_ORANGE : CLAUDE_TRACK);
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
#define GAME_COUNT 4
const char *GAME_NAMES[] = {"Flappy Clawd", "Block Drop", "2048", "Tilt Catch"};
const char *GAME_HINTS[] = {"OK / Shutter / UP: flap", "</> move  UP turn  DN fast  Shutter drop",
                            "D-pad: slide the tiles", "Tilt the board to steer"};
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
    int y = 40 + i * 40;
    bool sel = i == gameSelection;
    if (sel) ui->fillRoundRect(10, y, 220, 34, 8, CLAUDE_ORANGE);
    else ui->drawRoundRect(10, y, 220, 34, 8, CLAUDE_TRACK);
    ui->setTextSize(2);
    ui->setTextColor(sel ? CLAUDE_DARK : CLAUDE_CREAM);
    ui->setCursor(20, y + 9);
    ui->print(GAME_NAMES[i]);
    String best = "Best " + String(bestScores[i]);
    ui->setTextSize(1);
    ui->setTextColor(sel ? CLAUDE_DARK : CLAUDE_GRAY);
    ui->setCursor(222 - best.length() * 6, y + 13);
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
  if (gameOver || millis() - gameTick < 33) return;
  gameTick = millis();
  if (currentGame == GAME_FLAPPY) flappyUpdate();
  if (currentGame == GAME_TILT) tiltUpdate();
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
#define TOOL_COUNT 7
const char *TOOL_NAMES[] = {"Pomodoro", "Spirit Level", "Photo Booth", "Timelapse", "QR Scanner", "Home Controls", "Phone Remote"};
const char *TOOL_HINTS[] = {"Focus timer with breaks", "Is it straight?", "3-2-1 photo to the SD card",
                            "Record a timelapse, play it back", "Read QR codes (needs the bridge)",
                            "Your light, fan and robots", "Control me from your phone"};
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

// ---- QR scanner: frames go to the bridge, OpenCV reads the code ----
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
  drawCameraTitle(fb, qrBridgeDown ? "QR Scanner - bridge offline" : "QR Scanner");
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
if(s.usage)h+=`Session ${s.usage.session}%<div class=bar><i style="width:${s.usage.session}%"></i></div>Weekly ${s.usage.weekly}%<div class=bar><i style="width:${s.usage.weekly}%"></i></div>`;
h+=`Clawd: food ${s.pet.food}% &middot; energy ${s.pet.energy}% &middot; fun ${s.pet.fun}%${s.pet.nap?' &middot; napping':''}`;
$('st').innerHTML=h;const f=document.activeElement;
if(force||!f||!['INPUT','SELECT'].includes(f.tagName))$('al').innerHTML=s.alarms.map((a,i)=>`<div class=row>
<input type=time id=t${i} value="${p(a.h)}:${p(a.m)}"><select id=r${i}>${R.map((n,j)=>`<option value=${j}${j==a.r?' selected':''}>${n}</option>`).join('')}</select>
<label><input type=checkbox id=o${i}${a.on?' checked':''}> on</label><button onclick="save(${i})">Save</button></div>`).join('')}
load(true);setInterval(load,5000)
</script></body></html>)HTML";
const char *modeName() {
  switch (currentMode) {
    case MODE_USAGE: return "Claude Usage";
    case MODE_ASK: return "Claude Ask";
    case MODE_DANCE: return "Dance";
    case MODE_PET: return "Clawd Pet";
    case MODE_ALARMS: return "Alarms";
    case MODE_ALARM_RING: return "Alarm ringing!";
    case MODE_FACE: return "Face Finder";
    case MODE_WORLD: return "World Live";
    case MODE_GAME:
    case MODE_GAME_MENU: return "Games";
    case MODE_TOOL:
    case MODE_TOOLS: return "Toolbox";
    case MODE_SLEEP: return "Sleeping (clock)";
    case MODE_HOME_ASSISTANT: return "Home Assistant";
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
    doc["usage"]["session"] = (int)(usage.sessionPct + 0.5f);
    doc["usage"]["weekly"] = (int)(usage.weeklyPct + 0.5f);
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
  else webReply(c, 404, "text/plain", "not found");
  c.stop();
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
    case MODE_GAME:           // Tilt Catch is played without buttons
      return !(currentGame == GAME_TILT && !gameOver);
    case MODE_TOOL:           // hands-free tools, a running timer, or a recording
      return !((currentTool == TOOL_POMODORO && pomoRunning) || currentTool == TOOL_LEVEL || currentTool == TOOL_QR ||
               currentTool == TOOL_REMOTE || (currentTool == TOOL_TIMELAPSE && tlState != 0));
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
  // Quick usage peek, then how to wake up
  ui->setTextSize(1);
  ui->setTextColor(CLAUDE_GRAY);
  if (usage.valid) {
    char peek[40];
    snprintf(peek, sizeof(peek), "Session %d%%   Weekly %d%%", (int)(usage.sessionPct + 0.5f), (int)(usage.weeklyPct + 0.5f));
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
  // Give the quote a few seconds on screen, then always start on the Claude usage screen
  while (millis() - quoteShownAt < 4000) delay(10);
  currentMode = MODE_USAGE;
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
  webTick();
  static unsigned long lastEventCheck = 0;
  if (millis() - lastEventCheck > 1000) { // Telegram reminders, in every mode
    lastEventCheck = millis();
    checkScheduledEvents();
  }
  // Find out where we are once, for weather and planes
  if (!locationKnown && !locateTried && !netBusy() && WiFi.status() == WL_CONNECTED && currentMode != MODE_ASK) {
    locateTried = true;
    requestWidget(W_LOCATE);
  }
  // Keep usage fresh in the background (paused in Ask so questions aren't delayed, and in Dance)
  if (currentMode != MODE_ASK && currentMode != MODE_DANCE && currentMode != MODE_ALARM_RING && !netBusy() && (lastUsagePoll == 0 || millis() - lastUsagePoll > USAGE_POLL_MS)) {
    requestUsage(false);
  }
  if (pycamera.justPressed(AWEXP_BUTTON_SEL)) {
    if (currentMode != MODE_MENU) {
      stopDance();
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
    case MODE_ASK:
      askMode();
      break;
    case MODE_DANCE:
      danceMode();
      break;
    case MODE_SLEEP:
      sleepMode();
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
      case 0: currentMode = MODE_USAGE; break;
      case 1: currentMode = MODE_ASK; break;
      case 2: currentMode = MODE_DANCE; startDance(0); petBoost(5); break;
      case 3: currentMode = MODE_PET; break;
      case 4: currentMode = MODE_ALARMS; alarmField = -1; break;
      case 5: currentMode = MODE_FACE; break;
      case 6: currentMode = MODE_WORLD; break;
      case 7: currentMode = MODE_GAME_MENU; break;
      case 8: currentMode = MODE_TOOLS; break;
      case 9: currentMode = MODE_CAMERA; break;
      case 10: currentMode = MODE_HOME_ASSISTANT; break;
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