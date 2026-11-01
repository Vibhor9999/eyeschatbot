#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ================= SETTINGS =================
#define LAYOUT 0          // 0 = two OLEDs (one eye each)   1 = ONE OLED showing both eyes
#define TWO_BUSES 0       // LAYOUT 0 only: 0 = shared I2C bus (0x3C + 0x3D), 1 = separate buses
#define AUTO_MOODS 1      // 1 = random moods when idle, 0 = only change by command
#define AUTO_SLEEP_MS 0   // 0 = never; e.g. 90000 = tired after 90 s without commands, then sleep

#define SCREEN_W 128
#define SCREEN_H 64
#define SDA_PIN  21
#define SCL_PIN  22       // your SCL / SCK pin
#define SDA2_PIN 25       // only used when TWO_BUSES = 1
#define SCL2_PIN 26

#define ADDR_LEFT  0x3C
#if TWO_BUSES
  #define ADDR_RIGHT 0x3C
#else
  #define ADDR_RIGHT 0x3D
#endif

#if LAYOUT == 0
  #define EYE_HALFW   44      // half width of the eye
  #define IRIS_SCALE  1.05f
  #define LOOK_X      22      // how far the iris can move sideways
  #define LOOK_Y      6
  #define CONV_PX     3       // how much the irises turn toward the nose when looking at a person
  #define ZX          108     // position of the sleeping "z"
  #define ZY          4
#else
  #define EYE_HALFW   27
  #define IRIS_SCALE  0.85f
  #define LOOK_X      9
  #define LOOK_Y      5
  #define CONV_PX     2
  #define ZX          110
  #define ZY          0
#endif
#define EYE_CY 36             // vertical centre of the eye
// ============================================

Adafruit_SSD1306 dispL(SCREEN_W, SCREEN_H, &Wire, -1);
#if LAYOUT == 0
  #if TWO_BUSES
  Adafruit_SSD1306 dispR(SCREEN_W, SCREEN_H, &Wire1, -1);
  #else
  Adafruit_SSD1306 dispR(SCREEN_W, SCREEN_H, &Wire, -1);
  #endif
#endif

enum Mood { NORMAL, HAPPY, ANGRY, SAD, TIRED, SURPRISED, LOVE, WINK, SLEEP, DIZZY };
Mood mood = NORMAL;

struct Face {
  float Hu, Hl, iris, pupil, angry, sad, tired, happy, browY, browTilt;
};
Face cur = {24, 18, 16, 7, 0, 0, 0, 0, 8, 0};
Face tgt = {24, 18, 16, 7, 0, 0, 0, 0, 8, 0};

// gaze
float lookX = 0, lookY = 0, tgtLookX = 0, tgtLookY = 0, finalX = 0, finalY = 0;
float conv = 0, convTgt = 0;
bool  nearGaze = false;
int   jitX = 0, jitY = 0;
unsigned long settleAt = 0, lookHoldUntil = 0;

// eyelids
float openL = 1, openR = 1, openTgt = 1;
float openRateUp = 0.35f;
float winkCur = 1, winkTgt = 1;
unsigned long rateRestoreAt = 0;

// timers
unsigned long tFrame = 0, tBlink = 0, tBlinkEnd = 0, tLook = 0, tMood = 0, tHold = 0, tJit = 0;
unsigned long lastActivity = 0, sleepAt = 0;
bool blinking = false, doubleBlink = false;
int  idleStage = 0;   // 0 = active, 1 = tired, 2 = asleep

// ---------------- moods ----------------
void setMood(Mood m) {
  Mood prev = mood;
  mood = m;
  winkTgt = 1;

  if (m == SLEEP) {
    openTgt = 0.04f;
  } else if (prev == SLEEP) {          // slow wake-up
    openTgt = 1;
    openRateUp = 0.07f;
    rateRestoreAt = millis() + 1600;
    blinking = false;
    tBlink = millis() + 2500;
  }

  switch (m) {
    //                    Hu  Hl iris pup  angry  sad    tired happy  browY tilt
    case NORMAL:    tgt = Face{24, 18, 16, 7,  0,    0,     0,    0,     8,    0}; break;
    case HAPPY:     tgt = Face{24, 17, 16, 8,  0,    0,     0,    0.75f, 7,    0}; break;
    case ANGRY:     tgt = Face{24, 16, 15, 5,  0.7f, 0,     0,    0,     10,   7}; break;
    case SAD:       tgt = Face{24, 18, 17, 9,  0,    0.65f, 0,    0,     8,   -6}; break;
    case TIRED:     tgt = Face{22, 16, 16, 6,  0,    0,     0.5f, 0,     11,   0}; break;
    case SURPRISED: tgt = Face{29, 21, 13, 5,  0,    0,     0,    0,     3,    0}; break;
    case LOVE:      tgt = Face{24, 18, 17, 9,  0,    0,     0,    0.2f,  6,   -2}; break;
    case WINK:      tgt = Face{24, 18, 16, 7,  0,    0,     0,    0.3f,  7,    0}; winkTgt = 0.05f; break;
    case SLEEP:     tgt = Face{20, 14, 16, 6,  0,    0,     0.35f,0,     12,   0}; break;
    case DIZZY:     tgt = Face{22, 16, 16, 7,  0,    0,     0.2f, 0,     9,   -4}; break;
  }
}

// ---------------- gaze control ----------------
void saccadeTo(float nx, float ny) {          // quick jump with a small overshoot
  float dx = nx - lookX, dy = ny - lookY;
  tgtLookX = nx + dx * 0.12f;
  tgtLookY = ny + dy * 0.10f;
  finalX = nx; finalY = ny;
  settleAt = millis() + 80;
}

void lookAt(float x, float y, bool nearG, unsigned long holdMs) {
  if (mood == SLEEP) setMood(NORMAL);
  saccadeTo(x, y);
  nearGaze = nearG;
  convTgt = nearG ? CONV_PX : 0;
  lookHoldUntil = millis() + holdMs;
}

// ---------------- helpers ----------------
void drawHeart(Adafruit_SSD1306 &d, int cx, int cy, int r, uint16_t col) {
  d.fillCircle(cx - r, cy - r / 2, r, col);
  d.fillCircle(cx + r, cy - r / 2, r, col);
  d.fillTriangle(cx - 2 * r + 1, cy - r / 4, cx + 2 * r - 1, cy - r / 4, cx, cy + 2 * r, col);
}

// grey levels on a 1-colour display (4x4 ordered dither): L = 0 black ... 1 white
static const uint8_t BAYER[4][4] = {
  { 0,  8,  2, 10},
  {12,  4, 14,  6},
  { 3, 11,  1,  9},
  {15,  7, 13,  5}
};
static inline bool grey(int x, int y, float L) {
  return (int)(L * 16.0f) > BAYER[y & 3][x & 3];
}

void drawZzz(Adafruit_SSD1306 &d) {
  int n = (millis() / 600) % 4;               // 0..3 letters
  d.setTextColor(WHITE);
  d.setTextSize(1);
  if (n >= 1) { d.setCursor(ZX,      ZY + 14); d.print('z'); }
  if (n >= 2) { d.setCursor(ZX + 6,  ZY + 7);  d.print('z'); }
  if (n >= 3) { d.setCursor(ZX + 12, ZY);      d.print('Z'); }
}

// ---------------- draw one eye at position cx ----------------
void drawEyeAt(Adafruit_SSD1306 &d, int cx, int halfW, bool isLeft) {
  const float dir = isLeft ? 1.0f : -1.0f;   // +1: inner corner (nose side) on the right

  float o = isLeft ? openL : openR;
  if (mood == WINK && !isLeft && winkCur < o) o = winkCur;

  float gl = (mood == DIZZY) ? 0.0f : lookY;           // eyelids follow the gaze
  float cy = EYE_CY + (1.0f - o) * 6.0f;
  float Hu = (cur.Hu - gl * 0.45f + sinf(millis() / 700.0f) * 0.6f);
  if (Hu < 1) Hu = 1;
  Hu *= o;
  float Hl = (cur.Hl + gl * 0.25f);
  if (Hl < 1) Hl = 1;
  Hl *= (0.35f + 0.65f * o);

  static int topY[SCREEN_W], botY[SCREEN_W];
  int x0 = cx - halfW, x1 = cx + halfW;

  // 1) eye white: natural oval with lids, soft shadow under the upper lid
  for (int x = x0; x <= x1; x++) {
    float t = (x - cx) / (float)halfW;
    float ti = t * dir;                        // +1 inner, -1 outer
    float tilt = ti * 3.0f;                    // inner corner a bit lower
    float tu = (t - 0.15f * dir) / 1.15f;      // upper lid peak shifted toward the nose
    float su = 1.0f - tu * tu; if (su < 0) su = 0; su = powf(su, 0.6f);
    float l = 1.0f - t * t;    if (l < 0) l = 0;
    float sl = powf(l, 0.7f);
    float drop = (cur.angry * (0.5f + 0.5f * ti) +
                  cur.sad   * (0.5f - 0.5f * ti) +
                  cur.tired * 0.9f) * Hu;
    float cheek = cur.happy * (Hl + Hu * 0.55f) * powf(l, 0.7f);

    topY[x] = (int)(cy - Hu * su + tilt + drop + 0.5f);
    botY[x] = (int)(cy + Hl * sl + tilt - cheek + 0.5f);

    if (botY[x] >= topY[x]) {
      d.drawFastVLine(x, topY[x], botY[x] - topY[x] + 1, WHITE);
      for (int k = 0; k < 3; k++) {            // soft shadow under the upper lid
        int y = topY[x] + k;
        if (y > botY[x]) break;
        if (!grey(x, y, 0.30f + 0.25f * k)) d.drawPixel(x, y, BLACK);
      }
    }
  }

  // 2) iris: dark grey with fibres, black outer ring, black pupil
  int R = (int)(cur.iris * IRIS_SCALE + 0.5f);
  float flutter = sinf(millis() / 900.0f) * 0.8f + sinf(millis() / 230.0f) * 0.4f + sinf(millis() / 97.0f) * 0.25f;
  int pr = (int)(cur.pupil * IRIS_SCALE + conv * 0.3f + flutter + 0.5f);
  if (pr < 2) pr = 2;
  if (pr > R - 4) pr = R - 4;

  int ix, iy;
  if (mood == DIZZY) {                          // eyes circle around
    float ph = millis() / 180.0f + (isLeft ? 0.0f : 1.7f);
    ix = cx + (int)(cosf(ph) * LOOK_X * 0.7f);
    iy = (int)(cy + sinf(ph) * LOOK_Y * 0.8f);
  } else {
    float drift = sinf(millis() / 1700.0f + (isLeft ? 0.0f : 0.6f)) * 0.8f;
    ix = cx + (int)(lookX + drift + dir * conv) + jitX;     // dir*conv: irises turn toward the nose
    iy = (int)(cy + lookY - cur.happy * 4) + jitY;
  }
  float span = (float)max(1, R - 2 - pr);

  for (int py = iy - R; py <= iy + R; py++) {
    for (int px = ix - R; px <= ix + R; px++) {
      if (px < x0 || px > x1 || py < 0 || py >= SCREEN_H) continue;
      int dx = px - ix, dy = py - iy;
      int r2 = dx * dx + dy * dy;
      if (r2 > R * R) continue;
      float r = sqrtf((float)r2);
      bool white;
      if (r >= R - 2 || r <= pr) {
        white = false;                                           // outer ring + pupil
      } else {
        float t = (r - pr) / span;                               // 0 at pupil, 1 at edge
        float fibre = sinf(atan2f((float)dy, (float)dx) * 14.0f + t * 2.0f) * 0.5f + 0.5f;
        float L = 0.22f + 0.30f * (1.0f - t) + (fibre - 0.5f) * 0.35f;
        if (L < 0) L = 0;
        if (L > 1) L = 1;
        white = grey(px, py, L);
      }
      d.drawPixel(px, py, white ? WHITE : BLACK);
    }
  }

  // 3) reflections: they move less than the iris, like light on a real cornea
  if (mood == LOVE) {
    int s = 3 + ((int)(millis() / 180) % 2);
    drawHeart(d, ix, iy - 1, s, WHITE);
  } else {
    int rx = cx + (int)((ix - cx) * 0.8f) - R / 3;
    int ry = (int)(cy + (iy - cy) * 0.8f) - R / 3;
    d.fillCircle(rx, ry, R / 4 + 1, WHITE);
    d.fillCircle(rx + R / 2 + 1, ry + R * 2 / 3, 2, WHITE);
    if (mood == SAD) d.fillCircle(rx + 2, ry + R / 2, 1, WHITE);   // watery shine
  }

  // 4) clip everything outside the eye opening
  for (int x = x0; x <= x1; x++) {
    if (botY[x] >= topY[x]) {
      if (topY[x] > 0) d.drawFastVLine(x, 0, topY[x], BLACK);
      if (botY[x] < SCREEN_H - 1) d.drawFastVLine(x, botY[x] + 1, SCREEN_H - 1 - botY[x], BLACK);
    } else {
      d.drawFastVLine(x, 0, SCREEN_H, BLACK);
    }
  }

  // 5) eyelid lines
  for (int x = x0; x <= x1; x++) {
    if (botY[x] >= topY[x]) {
      d.drawPixel(x, topY[x] - 2, WHITE);
      d.drawPixel(x, topY[x] - 3, WHITE);
      if ((x & 1) == 0) d.drawPixel(x, botY[x] + 2, WHITE);
    }
  }

  // 6) eyelashes at the outer corner
  float fr[3] = {0.55f, 0.75f, 0.92f};
  for (int i = 0; i < 3; i++) {
    int lx = cx - (int)(dir * fr[i] * halfW);
    if (botY[lx] >= topY[lx])
      d.drawLine(lx, topY[lx] - 3, lx - (int)dir * (2 + i * 2), topY[lx] - 7 - i, WHITE);
  }

  // 7) eyebrow (lifts a little when looking up)
  float by = cur.browY + (1.0f - o) * 2.0f + gl * 0.3f;
  for (int x = cx - halfW + 4; x <= cx + halfW - 4; x++) {
    float t = (x - cx) / (float)(halfW - 4);
    float ti = t * dir;
    float y = by - 3.0f * (1.0f - t * t) + cur.browTilt * (0.5f + 0.5f * ti);
    int th = 2 + (ti > -0.3f) + (ti > 0.4f);
    d.drawFastVLine(x, (int)y, th, WHITE);
  }

  // 8) tear drop when sad
  if (mood == SAD && o > 0.5f) {
    float ph = fmodf(millis() / 2600.0f, 1.0f);
    if (ph > 0.25f) {
      float p = (ph - 0.25f) / 0.75f;
      int tx = cx - (int)(dir * 0.5f * halfW);
      if (botY[tx] >= topY[tx]) {
        int ty = botY[tx] + 3 + (int)(p * 5);
        if (ty < SCREEN_H - 3) {
          d.fillCircle(tx, ty, 2, WHITE);
          d.fillTriangle(tx - 2, ty - 1, tx + 2, ty - 1, tx, ty - 5, WHITE);
        }
      }
    }
  }
}

// ---------------- animation logic ----------------
void updateAnimation() {
  unsigned long now = millis();

  // ease the face shape
  float *c = (float *)&cur, *t = (float *)&tgt;
  for (int i = 0; i < 10; i++) c[i] += (t[i] - c[i]) * 0.22f;

  // gaze: fast jumps, then settle
  if (settleAt && now > settleAt) { tgtLookX = finalX; tgtLookY = finalY; settleAt = 0; }
  lookX += (tgtLookX - lookX) * 0.5f;
  lookY += (tgtLookY - lookY) * 0.5f;
  conv  += (convTgt - conv) * 0.15f;

  // eyelids: closing is fast, opening slower; right eye lags a little behind
  float rate = (openTgt < openL) ? 0.6f : openRateUp;
  openL += (openTgt - openL) * rate;
  openR += (openL - openR) * 0.5f;
  winkCur += (winkTgt - winkCur) * 0.35f;
  if (rateRestoreAt && now > rateRestoreAt) { openRateUp = 0.35f; rateRestoreAt = 0; }

  // blinking (not while asleep)
  if (mood != SLEEP) {
    if (!blinking && now > tBlink) {
      blinking = true;
      openTgt = (random(100) < 15) ? 0.45f : 0.05f;          // sometimes only a half blink
      tBlinkEnd = now + random(80, 140);
      doubleBlink = (random(100) < 20);
    }
    if (blinking && now > tBlinkEnd) {
      openTgt = 1;
      blinking = false;
      tBlink = doubleBlink ? now + 150 : now + random(2200, 5500);
      doubleBlink = false;
    }
  }

  // glances
  if (mood == SLEEP) {
    tgtLookX = 0; tgtLookY = 0; finalX = 0; finalY = 0; settleAt = 0; convTgt = 0;
  } else if (mood != DIZZY && now > tLook && now > lookHoldUntil) {
    float nx, ny;
    bool nearG = false;
    if (mood == SURPRISED || mood == ANGRY) {
      nx = random(-LOOK_X / 4, LOOK_X / 4 + 1); ny = random(-LOOK_Y / 2, LOOK_Y / 2 + 1);
    } else if (random(100) < 25) {
      nx = 0; ny = 0; nearG = true;                          // look straight at the customer
    } else {
      nx = random(-LOOK_X, LOOK_X + 1); ny = random(-LOOK_Y, LOOK_Y + 1);
    }
    if (fabsf(nx - lookX) > LOOK_X * 0.8f && !blinking && random(100) < 40) tBlink = now;  // blink on big moves
    nearGaze = nearG;
    convTgt = nearG ? CONV_PX : 0;
    saccadeTo(nx, ny);
    tLook = now + random(800, 2800);
  }

  // tiny eye tremor
  if (now - tJit > 140) {
    tJit = now;
    if (mood == SURPRISED || mood == SLEEP) { jitX = 0; jitY = 0; }
    else { jitX = random(-1, 2); jitY = random(-1, 2); }
  }

  // idle: tired, then asleep
#if AUTO_SLEEP_MS > 0
  if (idleStage == 0 && mood != SLEEP && now - lastActivity > AUTO_SLEEP_MS) {
    setMood(TIRED);
    idleStage = 1;
    sleepAt = now + 7000;
  } else if (idleStage == 1 && now > sleepAt) {
    setMood(SLEEP);
    idleStage = 2;
  }
#endif

  // automatic mood changes
#if AUTO_MOODS
  if (idleStage == 0 && mood != SLEEP && mood != DIZZY && now > tMood && now > tHold) {
    int p = random(100);
    Mood m = NORMAL;
    if (p < 35) m = NORMAL;
    else if (p < 55) m = HAPPY;
    else if (p < 63) m = LOVE;
    else if (p < 72) m = WINK;
    else if (p < 80) m = SURPRISED;
    else if (p < 88) m = TIRED;
    else if (p < 94) m = SAD;
    else m = ANGRY;
    setMood(m);
    tMood = now + random(4000, 8000);
  }
#endif
}

// ---------------- serial control ----------------
// moods:  n normal  h happy  a angry  s sad  t tired  u surprised  l love  w wink  d dizzy  z sleep  x wake
// gaze:   c look at customer  g look at table  j look left  k look right
// other:  b blink
void userMood(Mood m) { setMood(m); tHold = millis() + 8000; }

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    bool known = true;
    switch (c) {
      case 'n': userMood(NORMAL);    break;
      case 'h': userMood(HAPPY);     break;
      case 'a': userMood(ANGRY);     break;
      case 's': userMood(SAD);       break;
      case 't': userMood(TIRED);     break;
      case 'u': userMood(SURPRISED); break;
      case 'l': userMood(LOVE);      break;
      case 'w': userMood(WINK);      break;
      case 'd': userMood(DIZZY);     break;
      case 'z': userMood(SLEEP);     break;
      case 'x': userMood(NORMAL);    break;
      case 'b': tBlink = 0;          break;
      case 'c': lookAt(0, -2, true, 5000);       break;
      case 'g': lookAt(0, LOOK_Y, false, 4000);  break;
      case 'j': lookAt(-LOOK_X, 0, false, 3000); break;
      case 'k': lookAt(LOOK_X, 0, false, 3000);  break;
      default: known = false;
    }
    if (known) { lastActivity = millis(); idleStage = 0; }
  }
}

// ---------------- eyes setup / frame (called from the chatbot part below) ----------------
void eyesBegin() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
#if LAYOUT == 0 && TWO_BUSES
  Wire1.begin(SDA2_PIN, SCL2_PIN);
  Wire1.setClock(400000);
#endif
  if (!dispL.begin(SSD1306_SWITCHCAPVCC, ADDR_LEFT)) Serial.println("Left OLED not found");
#if LAYOUT == 0
  if (!dispR.begin(SSD1306_SWITCHCAPVCC, ADDR_RIGHT)) Serial.println("Right OLED not found");
#endif
  randomSeed(esp_random());
  setMood(NORMAL);
  tBlink = millis() + 2000;
  lastActivity = millis();
}

void eyesFrame() {
  handleSerial();
  if (millis() - tFrame >= 25) {
    tFrame = millis();
    updateAnimation();
#if LAYOUT == 0
    dispL.clearDisplay();
    drawEyeAt(dispL, 64, EYE_HALFW, true);
    dispL.display();
    dispR.clearDisplay();
    drawEyeAt(dispR, 64, EYE_HALFW, false);
    if (mood == SLEEP) drawZzz(dispR);
    dispR.display();
#else
    dispL.clearDisplay();
    drawEyeAt(dispL, 32, EYE_HALFW, true);     // left eye
    drawEyeAt(dispL, 96, EYE_HALFW, false);    // right eye
    if (mood == SLEEP) drawZzz(dispL);
    dispL.display();
#endif
  }
}

// ============================================================
//  CHATBOT PART (cloud version - no PC needed)
//  INMP441 mic -> ESP32 -> WiFi/HTTPS -> your server on the internet
//  (speech-to-text + AI + voice)  ->  raw 16 kHz audio  ->  MAX98357A -> speaker
//  The eyes run on their own core, so they keep moving all the time.
// ============================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>

// ---------------- USER SETTINGS ----------------
#define WIFI_SSID    "YOUR_WIFI_NAME"
#define WIFI_PASS    "YOUR_WIFI_PASSWORD"

#define SERVER_HOST  "your-robot.onrender.com"   // your server address, WITHOUT https://
#define SERVER_PORT  443
#define USE_HTTPS    1                           // 1 = internet server (https)   0 = PC on your WiFi (http)
#define DEVICE_TOKEN "change-this-secret"        // must be the same as DEVICE_TOKEN on the server
#define KEEPALIVE_MS 540000UL                    // wake the free server every 9 minutes so it never sleeps

// INMP441 microphone
#define MIC_SCK      32
#define MIC_WS       33
#define MIC_SD       35     // data out of mic (L/R pin of mic goes to GND)
// MAX98357A amplifier
#define SPK_BCLK     18
#define SPK_LRC      19
#define SPK_DIN      23

// audio tuning
#define SAMPLE_RATE      16000
#define MIC_SHIFT        14      // lower = louder mic (13, 12 ...), higher = quieter (15, 16)
#define VOLUME           0.8f    // 0.0 - 1.0
#define MIN_SPEECH_LEVEL 1000    // minimum loudness that counts as speech
#define START_BLOCKS     3       // loud blocks in a row to start (1 block = 32 ms)
#define PRE_BLOCKS       6       // audio kept from before you started talking
#define SILENCE_END_MS   1200    // stop recording after this much silence
#define NO_SPEECH_MS     5000    // give up if nobody speaks
#define MAX_REC_MS       12000   // longest question
#define DEBUG_LEVEL      0       // 1 = print mic level to Serial Monitor (for tuning)
// ------------------------------------------------

static const i2s_port_t MIC_PORT = I2S_NUM_0;
static const i2s_port_t SPK_PORT = I2S_NUM_1;

#if USE_HTTPS
WiFiClientSecure client;
#else
WiFiClient client;
#endif
float noiseLevel = 100;
unsigned long lastPing = 0;

// microphone ring buffer (2 seconds): the mic is always recording into it, so nothing is
// lost while the secure connection to the server is being opened
#define RING_SAMPLES 32768                       // must be a power of two
static int16_t *ring = nullptr;
static volatile uint32_t wrPos = 0;              // total samples written
static uint32_t rdPos = 0;                       // total samples read

// ---------- eyes helpers ----------
void face(int m, unsigned long holdMs) {          // set a mood and stop random moods for a while
  setMood((Mood)m);
  tHold = millis() + holdMs;
  lastActivity = millis();
  idleStage = 0;
}

int moodFromName(String s) {
  s.trim(); s.toLowerCase();
  if (s == "normal")    return NORMAL;
  if (s == "happy")     return HAPPY;
  if (s == "sad")       return SAD;
  if (s == "surprised") return SURPRISED;
  if (s == "angry")     return ANGRY;
  if (s == "tired")     return TIRED;
  if (s == "love")      return LOVE;
  if (s == "wink")      return WINK;
  return HAPPY;
}

void eyesTask(void *) {                            // runs on core 0
  for (;;) {
    eyesFrame();
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}

// ---------- audio ----------
void audioBegin() {
  // microphone (RX)
  i2s_config_t mic = {};
  mic.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  mic.sample_rate = SAMPLE_RATE;
  mic.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  mic.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  mic.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  mic.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  mic.dma_buf_count = 8;
  mic.dma_buf_len = 256;
  i2s_pin_config_t micPins = {};
  micPins.mck_io_num = I2S_PIN_NO_CHANGE;
  micPins.bck_io_num = MIC_SCK;
  micPins.ws_io_num = MIC_WS;
  micPins.data_out_num = I2S_PIN_NO_CHANGE;
  micPins.data_in_num = MIC_SD;
  i2s_driver_install(MIC_PORT, &mic, 0, NULL);
  i2s_set_pin(MIC_PORT, &micPins);

  // speaker (TX) - stereo, same sample on both channels so the amp gives full volume
  i2s_config_t spk = {};
  spk.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  spk.sample_rate = SAMPLE_RATE;
  spk.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  spk.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  spk.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  spk.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  spk.dma_buf_count = 8;
  spk.dma_buf_len = 512;
  spk.tx_desc_auto_clear = true;
  i2s_pin_config_t spkPins = {};
  spkPins.mck_io_num = I2S_PIN_NO_CHANGE;
  spkPins.bck_io_num = SPK_BCLK;
  spkPins.ws_io_num = SPK_LRC;
  spkPins.data_out_num = SPK_DIN;
  spkPins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_driver_install(SPK_PORT, &spk, 0, NULL);
  i2s_set_pin(SPK_PORT, &spkPins);
  i2s_zero_dma_buffer(SPK_PORT);
}

// low level: read n samples (n <= 512) from the mic as 16-bit (used only by micTask)
int readMic(int16_t *out, int n, int &level) {
  static int32_t raw[512];
  static float dc = 0;
  static bool dcInit = false;
  size_t got = 0;
  i2s_read(MIC_PORT, raw, n * 4, &got, pdMS_TO_TICKS(100));
  int cnt = got / 4;
  long sum = 0;
  for (int i = 0; i < cnt; i++) {
    int32_t v = raw[i] >> MIC_SHIFT;
    if (!dcInit) { dc = v; dcInit = true; }
    dc += (v - dc) * 0.002f;                      // remove the mic's constant DC offset
    v -= (int32_t)dc;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    out[i] = (int16_t)v;
    sum += abs(v);
  }
  level = cnt ? (int)(sum / cnt) : 0;
  return cnt;
}

void micTask(void *) {                             // keeps recording into the ring buffer, always
  static int16_t b[512];
  int l;
  for (;;) {
    int n = readMic(b, 512, l);
    if (n <= 0) continue;
    uint32_t w = wrPos;
    for (int i = 0; i < n; i++) ring[(w + i) & (RING_SAMPLES - 1)] = b[i];
    wrPos = w + n;
  }
}

// get the next 512 samples (waits if needed), returns their average loudness
int getBlock(int16_t *out, int &level) {
  while ((uint32_t)(wrPos - rdPos) < 512) delay(1);
  uint32_t avail = wrPos - rdPos;
  if (avail > RING_SAMPLES - 2048) {              // we fell behind: skip the oldest audio
    rdPos = wrPos - (RING_SAMPLES - 4096);
  }
  long sum = 0;
  for (int i = 0; i < 512; i++) {
    int16_t v = ring[(rdPos + i) & (RING_SAMPLES - 1)];
    out[i] = v;
    sum += abs(v);
  }
  rdPos += 512;
  level = (int)(sum / 512);
  return 512;
}

void flushMic() { rdPos = wrPos; }

float speechThreshold() {
  float t = noiseLevel * 2.5f;
  return t < MIN_SPEECH_LEVEL ? MIN_SPEECH_LEVEL : t;
}

void calibrateNoise() {
  static int16_t b[512];
  int l, lv[40], k = 0;
  delay(800);                                    // let the mic and filter settle
  flushMic();
  for (int i = 0; i < 40; i++) {                 // ~1.3 s of room noise
    getBlock(b, l);
    if (i > 3) lv[k++] = l;
  }
  for (int i = 1; i < k; i++) {                  // sort
    int v = lv[i], j = i - 1;
    while (j >= 0 && lv[j] > v) { lv[j + 1] = lv[j]; j--; }
    lv[j + 1] = v;
  }
  float acc = 0;
  int half = k / 2 > 0 ? k / 2 : 1;
  for (int i = 0; i < half; i++) acc += lv[i];   // average of the quietest half = real background level
  if (k) noiseLevel = acc / half;
  Serial.printf("Room noise level: %.0f  (speech threshold %.0f)\n", noiseLevel, speechThreshold());
}

// ---------- server helpers ----------
// open the connection to the server
bool serverConnect() {
  client.stop();
  return client.connect(SERVER_HOST, SERVER_PORT);
}

// ask the server "are you awake?" - also keeps a free server from going to sleep
bool pingServer(unsigned long timeoutMs) {
  if (!serverConnect()) return false;
  client.print("GET /health HTTP/1.1\r\nHost: " SERVER_HOST "\r\nConnection: close\r\n\r\n");
  unsigned long t = millis();
  while (!client.available() && client.connected() && millis() - t < timeoutMs) delay(20);
  bool ok = false;
  if (client.available()) ok = client.readStringUntil('\n').indexOf("200") > 0;
  client.stop();
  lastPing = millis();
  return ok;
}

// wait until somebody starts talking (voice trigger). Returns with the ring buffer rewound
// so the first words are included.
bool waitTrigger() {
  static int16_t b[512];
  int lvl, loud = 0;
  unsigned long dbg = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (millis() - lastPing > KEEPALIVE_MS) { pingServer(15000); flushMic(); loud = 0; }
    getBlock(b, lvl);
#if DEBUG_LEVEL
    if (millis() - dbg > 500) { dbg = millis(); Serial.printf("mic level %d  thr %.0f\n", lvl, speechThreshold()); }
#endif
    if (lvl > speechThreshold()) loud++;
    else { loud = 0; noiseLevel = noiseLevel * 0.95f + lvl * 0.05f; }
    if (loud >= START_BLOCKS) {
      uint32_t back = (uint32_t)(START_BLOCKS + PRE_BLOCKS) * 512;     // go back a little: keep the start of the sentence
      uint32_t avail = wrPos - rdPos;
      uint32_t room = (RING_SAMPLES - 4096 > avail) ? (RING_SAMPLES - 4096 - avail) : 0;
      if (back > room) back = room;
      rdPos -= back;
      return true;
    }
  }
}

// ---------- reading the server's answer (handles both Content-Length and chunked) ----------
static long bodyLeft = 0;
static bool bodyChunked = false;
static bool bodyEnd = false;

// returns: >0 bytes read, 0 = nothing yet, -1 = finished
int bodyRead(uint8_t *buf, int want) {
  if (bodyEnd) return -1;
  if (bodyChunked) {
    if (bodyLeft == 0) {
      if (!client.available()) {
        if (!client.connected()) { bodyEnd = true; return -1; }
        return 0;
      }
      String l = client.readStringUntil('\n');
      l.trim();
      if (l.length() == 0) return 0;                       // blank line between chunks
      bodyLeft = strtol(l.c_str(), NULL, 16);
      if (bodyLeft == 0) { bodyEnd = true; return -1; }
    }
    int n = client.read(buf, (int)min((long)want, bodyLeft));
    if (n > 0) { bodyLeft -= n; return n; }
    return 0;
  }
  if (bodyLeft <= 0) { bodyEnd = true; return -1; }
  int n = client.read(buf, (int)min((long)want, bodyLeft));
  if (n > 0) { bodyLeft -= n; return n; }
  if (!client.connected() && !client.available()) { bodyEnd = true; return -1; }
  return 0;
}

// play raw 16 kHz mono 16-bit PCM coming from the server
void playPcm() {
  static uint8_t raw[1024];
  static int16_t st[1024];
  int have = 0;
  unsigned long last = millis();
  for (;;) {
    int n = bodyRead(raw + have, (int)sizeof(raw) - have);
    if (n < 0) break;
    if (n == 0) {
      if (millis() - last > 8000) break;
      delay(2);
      continue;
    }
    last = millis();
    have += n;
    int s = have / 2;
    for (int i = 0; i < s; i++) {
      int16_t v = (int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
      v = (int16_t)(v * VOLUME);
      st[2 * i] = v;
      st[2 * i + 1] = v;
    }
    size_t wr;
    i2s_write(SPK_PORT, st, s * 4, &wr, portMAX_DELAY);
    if (have & 1) raw[0] = raw[have - 1];
    have &= 1;
  }
  static int16_t z[512] = {0};                   // flush with silence so the last word is not cut
  size_t wr;
  for (int i = 0; i < 3; i++) i2s_write(SPK_PORT, z, sizeof(z), &wr, portMAX_DELAY);
  i2s_zero_dma_buffer(SPK_PORT);
}

// send one block of 16-bit samples as an HTTP chunk
void sendBlock(const int16_t *samples, int n) {
  static uint8_t pkt[1040];
  int bytes = n * 2;
  int h = snprintf((char *)pkt, 16, "%X\r\n", bytes);
  memcpy(pkt + h, samples, bytes);
  pkt[h + bytes] = '\r';
  pkt[h + bytes + 1] = '\n';
  client.write(pkt, h + bytes + 2);
}

// ---------- one full question -> answer ----------
void converse() {
  static int16_t blk[512];

  face(NORMAL, 60000);
  lookAt(0, -2, true, 60000);                    // listening: look straight at the person

  // open the secure connection (the mic keeps recording into the ring buffer meanwhile)
  if (!serverConnect()) {
    Serial.println("Cannot reach server");
    face(SAD, 2500);
    flushMic();
    return;
  }
  client.print("POST /chat HTTP/1.1\r\nHost: " SERVER_HOST "\r\n"
               "X-Device-Token: " DEVICE_TOKEN "\r\n"
               "Content-Type: application/octet-stream\r\n"
               "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n");

  // --- stream the question to the server until the person stops talking ---
  const float thr = speechThreshold();
  const int blockMs = 32;
  int silentBlocks = 0, totalBlocks = 0;
  for (;;) {
    int lvl;
    getBlock(blk, lvl);
    sendBlock(blk, 512);
    totalBlocks++;
    if (lvl > thr) silentBlocks = 0; else silentBlocks++;
    if (silentBlocks * blockMs > SILENCE_END_MS) break;
    if (totalBlocks * blockMs > MAX_REC_MS) break;
    if (!client.connected()) break;
  }
  client.print("0\r\n\r\n");

  // --- thinking: eyes go dizzy while the server does speech-to-text + AI + voice ---
  face(DIZZY, 90000);
  unsigned long w = millis();
  while (!client.available() && client.connected() && millis() - w < 60000) delay(10);

  String status = client.readStringUntil('\n');
  int code = status.substring(9, 12).toInt();
  long len = -1;
  bodyChunked = false;
  bodyEnd = false;
  String moodName = "happy";
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    line.toLowerCase();
    if (line.startsWith("content-length:")) len = line.substring(15).toInt();
    if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") > 0) bodyChunked = true;
    if (line.startsWith("x-mood:")) { moodName = line.substring(7); moodName.trim(); }
  }
  bodyLeft = bodyChunked ? 0 : len;

  if (code != 200 || (!bodyChunked && len <= 0)) {
    Serial.printf("No answer (HTTP %d)%s\n", code, code == 401 ? "  <- DEVICE_TOKEN does not match the server" : "");
    client.stop();
    face(code == 204 ? SURPRISED : SAD, 2000);   // 204 = server heard nothing
    flushMic();
    return;
  }

  // --- speaking ---
  face(moodFromName(moodName), 90000);
  lookAt(0, -2, true, 90000);
  playPcm();
  client.stop();

  lookHoldUntil = millis();
  face(NORMAL, 2500);
  lastPing = millis();                           // the server was just used, so it is awake
  delay(300);
  flushMic();                                    // do not listen to our own voice
}

// ---------- setup / loop ----------
void setup() {
  eyesBegin();                                   // your original eyes setup (Serial + displays)
  xTaskCreatePinnedToCore(eyesTask, "eyes", 10240, NULL, 1, NULL, 0);

  ring = (int16_t *)malloc(RING_SAMPLES * sizeof(int16_t));
  if (!ring) { Serial.println("Out of memory for the mic buffer!"); for (;;) delay(1000); }
  audioBegin();
  xTaskCreatePinnedToCore(micTask, "mic", 4096, NULL, 2, NULL, 1);

  face(TIRED, 300000);                           // sleepy eyes while connecting
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
  Serial.print("WiFi OK, IP: ");
  Serial.println(WiFi.localIP());

#if USE_HTTPS
  client.setInsecure();                          // no certificate check (simple); the secret token protects the server
#endif

  // wake the server (a free server can take up to a minute to start)
  for (int i = 0; i < 4; i++) {
    Serial.println("Waking up the server...");
    if (pingServer(70000)) { Serial.println("Server is ready"); break; }
    Serial.println("Server did not answer yet");
  }

  calibrateNoise();
  face(HAPPY, 3000);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    face(SAD, 3000);
    WiFi.reconnect();
    delay(2000);
    return;
  }
  if (waitTrigger()) {
    converse();
    delay(200);
  }
}
