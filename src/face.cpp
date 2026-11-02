// Sila - the face (home screen).
//
// Wide eyes with a soft halo, brows, a small mouth, status icons and a speech
// badge. Everything is drawn with shapes; every parameter eases toward its
// target so expressions morph smoothly.
//
// Each life stage has its own face:
//   BABY  - big round eyes, sparkles, rosy cheeks, no brows, pink halo
//   TEEN  - classic wide eyes and brows, one sparkle, teal halo
//   ADULT - wider, sharper eyes and thicker brows, blue halo
//   ELDER - smaller, slightly droopy eyes, bushy brows, round glasses, violet halo
//
// The face draws into faceCanvas (landscape or portrait); the UI rotates it.
// Sizes are written in landscape units and scaled by S at draw time.

#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#include "ui.h"
#include "ui_anim.h"
#include "theme.h"

using namespace theme;

// Both canvases are made once at boot and never freed: re-creating one on every
// landscape/portrait switch failed once WiFi/BLE had fragmented the RAM, and
// the face stayed black until a restart.
M5Canvas faceLand, facePort;
M5Canvas* faceCanvas = &faceLand;
static M5Canvas*& cv = faceCanvas;

float faceMotionX = 0, faceMotionY = 0;   // set by the UI: movement in face coordinates (g)

namespace {

enum MouthStyle { MOUTH_DASH, MOUTH_SMILE, MOUTH_BIG_SMILE, MOUTH_FROWN, MOUTH_O, MOUTH_YAWN,
                  MOUTH_CHEW, MOUTH_WAVY, MOUTH_NONE };

// ---------------- Stage looks ----------------
struct StageLook {
  float w, h, gap;
  int   radius;
  bool  brows;
  float browHalf, browThick;
  uint16_t browColor;
  bool  cheeks;
  int   sparkles;
  bool  glasses;
  float baseTired;
  uint16_t halo;
  MouthStyle restMouth;
};

const StageLook LOOKS[4] = {
  // w   h   gap  r   brows half thick colour                cheeks spark glasses tired halo                 mouth
  { 50, 50, 28, 20, false, 0,  0,   BROW,                  true,  2, false, 0.00f, rgb(70, 28, 56),  MOUTH_SMILE },  // BABY
  { 58, 42, 24, 12, true, 16, 1.4f, BROW,                  false, 0, false, 0.00f, EYE_GLOW,         MOUTH_DASH  },  // TEEN
  { 66, 36, 20,  7, true, 19, 2.1f, rgb(120, 175, 235),    false, 0, false, 0.00f, rgb(20, 40, 80),  MOUTH_DASH  },  // ADULT
  { 56, 34, 28, 10, true, 20, 2.5f, rgb(205, 205, 215),    false, 0, true,  0.16f, rgb(44, 30, 72),  MOUTH_DASH  },  // ELDER
};
const StageLook* L = &LOOKS[1];
float BASE_W = 58, BASE_H = 42, GAP = 24;

const int HALO = 3;

// ---------------- Runtime layout ----------------
int   W = SCR_W, H = SCR_H;
bool  portrait = false;
float S = 1.0f;
int   CX = SCR_W / 2, CY = 58, MOUTH_Y = 92, BADGE_Y = 109;

void layout() {
  W = cv->width(); H = cv->height();
  portrait = H > W;
  S  = portrait ? 0.72f : 1.0f;
  CX = W / 2;
  CY      = portrait ? (int)(H * 0.40f) : 58;
  MOUTH_Y = CY + (int)(34 * S);
  BADGE_Y = portrait ? CY + 62 : 109;

  L = &LOOKS[constrain((int)petStage, 0, 3)];
  BASE_W = L->w; BASE_H = L->h; GAP = L->gap;
}

struct FaceParams {
  float lw, lh, rw, rh;   // eye sizes
  float gx, gy;           // gaze offset (px)
  float tired;            // outer-corner droop      0..1
  float angry;            // inner-corner droop      0..1
  float flat;             // flat top lid            0..1
  float happy;            // bottom crescent (^ ^)   0..1
  float tilt;             // +ve raises the right eye (px)
  float browRaise;        // both brows up (+) / down (-) (px)
  float browSlant;        // +ve focused, -ve worried
  float browL, browR;     // extra raise per brow
  float pupil;            // focus pupils (hunting)
};

FaceParams cur, tgt;
bool       inited = false;

MouthStyle mouth     = MOUTH_DASH;
uint16_t   eyeColor  = EYE;
float      bounceAmp = 0;
float      jitter    = 0;
bool       blush     = false;
bool       radar     = false;
bool       showZzz   = false;
bool       wanderFast = false;
bool       sweepGaze  = false;
bool       allowBlink = true;
bool       dizzy      = false;
bool       sparkleBurst = false;
bool       startled   = false;
bool       earWaves   = false;   // listening to you (teal = you)
bool       notes      = false;   // dancing to music
float      danceBob   = 0;

// Intro (boot / after hatching): black, then the eyes pop in
unsigned long introStart = 0;
uint32_t      introBlack = 0;
const uint32_t INTRO_POP_MS = 520;
const uint32_t INTRO_MS     = 1900;   // after the black part

// Idle quirks
enum Quirk { Q_NONE, Q_WINK, Q_YAWN, Q_LOOK };
Quirk         quirk      = Q_NONE;
int           winkSide   = 0;
unsigned long quirkUntil = 0;
unsigned long nextQuirk  = 0;

// Evolution celebration
int           lastStage   = -1;
unsigned long evolveUntil = 0;

unsigned long lastFrame  = 0;
unsigned long nextBlink  = 0;
unsigned long blinkStart = 0;
bool          blinking   = false;
unsigned long nextGaze   = 0;
float         gazeTx = 0, gazeTy = 0;

unsigned long eatUntil   = 0;
bool          lastHungerEffect = false;
bool          lastScanning = false;
unsigned long badgeUntil  = 0;

const uint32_t BLINK_MS = 150;

float ease(float c, float t, float k) { return c + (t - c) * k; }
int   sc(float v) { return (int)(v * S); }

bool introRunning(unsigned long now) {
  return introStart != 0 && now - introStart < introBlack + INTRO_MS;
}

// ---------------------------------------------------------------------------
// Expression targets
// ---------------------------------------------------------------------------
void setNeutral() {
  tgt.lw = tgt.rw = BASE_W;
  tgt.lh = tgt.rh = BASE_H;
  tgt.tired = L->baseTired;
  tgt.angry = tgt.flat = tgt.happy = 0;
  tgt.tilt = tgt.browRaise = tgt.browSlant = tgt.browL = tgt.browR = 0;
  tgt.pupil = 0;
  mouth      = L->restMouth;
  eyeColor   = EYE;
  bounceAmp  = jitter = 0;
  blush = radar = showZzz = dizzy = sparkleBurst = false;
  startled = earWaves = notes = false; danceBob = 0;
  wanderFast = sweepGaze = false;
  allowBlink = true;
}

// After the pop: a blink, a glance left and right, then normal life
bool introScript(unsigned long now) {
  if (!introRunning(now)) return false;
  unsigned long t = now - introStart;
  if (t < introBlack) return true;               // still black
  t -= introBlack;
  allowBlink = false;
  tgt.browRaise = 3;
  if (t > 750 && t < 900) tgt.lh = tgt.rh = 4;   // first blink
  if (t > 1000 && t < 1350) gazeTx = -16;
  else if (t > 1350 && t < 1700) gazeTx = 16;
  else gazeTx = 0;
  gazeTy = 0;
  return true;
}

void pickQuirk(unsigned long now) {
  if (quirk != Q_NONE && now >= quirkUntil) quirk = Q_NONE;
  if (quirk != Q_NONE || now < nextQuirk) return;
  nextQuirk = now + random(15000, 35000);
  int r = random(0, 3);
  if (r == 0)      { quirk = Q_WINK; winkSide = random(0, 2) ? 1 : -1; quirkUntil = now + 550; }
  else if (r == 1) { quirk = Q_YAWN; quirkUntil = now + 1500; }
  else             { quirk = Q_LOOK; quirkUntil = now + 1700; }
}

void pickExpression(unsigned long now) {
  setNeutral();
  if (introScript(now)) return;

  if (hungerEffectActive && !lastHungerEffect) eatUntil = now + 1600;
  lastHungerEffect = hungerEffectActive;

  if (lastStage < 0) lastStage = petStage;
  if ((int)petStage != lastStage) { lastStage = petStage; evolveUntil = now + 4500; }

  // ---- Sleeping ----
  if (currentActivity == ACT_REST && restPhase != REST_NONE) {
    float closed = (restPhase == REST_DEEP) ? 1.0f : (4 - constrain(restFrameIndex, 0, 4)) / 4.0f;
    tgt.lh = tgt.rh = BASE_H - (BASE_H - 4) * closed;
    tgt.browRaise = -5 * closed;
    mouth      = closed > 0.8f ? MOUTH_NONE : MOUTH_DASH;
    showZzz    = restPhase == REST_DEEP;
    allowBlink = false;
    eyeColor   = closed > 0.6f ? EYE_SLEEP : EYE;   // turn sleepy-blue only once mostly closed
    if (now < stirUntil && restPhase == REST_DEEP) {      // stirs: eyes crack open, mumbles
      tgt.lh = tgt.rh = 9; tgt.flat = 0.3f; tgt.browSlant = -0.3f; mouth = MOUTH_WAVY; showZzz = false;
    }
    return;
  }

  // ---- Falling: screams ----
  if (now < fallUntil) {
    startled = true;
    tgt.lw = tgt.rw = BASE_W + 8; tgt.lh = tgt.rh = BASE_H + 14;
    tgt.browRaise = 10; tgt.pupil = 0.8f;
    mouth = MOUTH_YAWN; jitter = 2.5f; allowBlink = false;
    return;
  }

  // ---- Landed hard: squeezed eyes (then dizzy) ----
  if (now < ouchUntil) {
    tgt.lh = tgt.rh = 5; tgt.angry = 0.4f; tgt.browSlant = -0.8f;
    mouth = MOUTH_WAVY; jitter = 1.0f; allowBlink = false;
    return;
  }

  // ---- Tossed and caught: loved it ----
  if (now < wheeUntil) {
    tgt.happy = 0.7f; tgt.lh = tgt.rh = BASE_H + 8; tgt.browRaise = 6;
    mouth = MOUTH_BIG_SMILE; bounceAmp = 6; sparkleBurst = true; allowBlink = false;
    return;
  }

  // ---- Just evolved ----
  if (now < evolveUntil) {
    tgt.happy = 0.5f; tgt.browRaise = 5;
    tgt.lh = tgt.rh = BASE_H + 6;
    mouth = MOUTH_BIG_SMILE;
    sparkleBurst = true;
    bounceAmp = 3;
    allowBlink = false;
    return;
  }

  // ---- Startled (loud bang / woken by a hard bump) ----
  if (now < startleUntil) {
    startled = true;
    tgt.lw = tgt.rw = BASE_W + 6; tgt.lh = tgt.rh = BASE_H + 12;
    tgt.browRaise = 9; tgt.pupil = 0.55f;
    mouth = MOUTH_O; jitter = (startleUntil - now) > 1800 ? 2.0f : 0.6f;
    allowBlink = false;
    return;
  }

  // ---- Daily step goal reached ----
  if (now < stepGoalUntil) {
    tgt.happy = 0.6f; tgt.browRaise = 5; mouth = MOUTH_BIG_SMILE;
    sparkleBurst = true; bounceAmp = 4; allowBlink = false;
    return;
  }

  // ---- Dizzy ----
  if (now < dizzyUntil) {
    dizzy = true;
    tgt.lh = tgt.rh = BASE_H + 6;
    tgt.browSlant = -0.5f;
    mouth = MOUTH_WAVY;
    allowBlink = false;
    return;
  }

  // ---- Hunting WiFi ----
  if (currentActivity == ACT_HUNT) {
    tgt.lh = tgt.rh = BASE_H - 6;
    tgt.angry     = 0.22f;
    tgt.browSlant = 0.5f;
    tgt.pupil     = 1;
    radar = true; sweepGaze = true; allowBlink = false;
    return;
  }

  // ---- Discovering ----
  if (currentActivity == ACT_DISCOVER) {
    tgt.lh = BASE_H + 8; tgt.rh = BASE_H - 4; tgt.browL = 5;
    mouth = MOUTH_O; wanderFast = true;
    return;
  }

  // ---- Eating ----
  if (now < eatUntil) {
    tgt.happy = 0.45f; tgt.browRaise = 3;
    mouth = MOUTH_CHEW; allowBlink = false;
    return;
  }

  // ---- Playing / patted ----
  if (now < playEffectUntil) {
    tgt.happy = 0.6f; tgt.lh = tgt.rh = BASE_H + 6; tgt.browRaise = 5;
    mouth = MOUTH_BIG_SMILE; bounceAmp = 5; allowBlink = false;
    return;
  }

  // ---- Double clap: "you called?" - big eyes on you, happy ----
  if (now < calledUntil) {
    tgt.lw = tgt.rw = BASE_W + 4; tgt.lh = tgt.rh = BASE_H + 8;
    tgt.happy = 0.3f; tgt.browRaise = 6; tgt.browL = 2;
    mouth = MOUTH_SMILE; bounceAmp = 2;
    return;
  }

  // ---- Just plugged in: eats electricity ----
  if (now < zapUntil) {
    tgt.happy = 0.5f; tgt.browRaise = 4;
    mouth = (now / 160) % 2 ? MOUTH_CHEW : MOUTH_BIG_SMILE;
    jitter = (zapUntil - now) > 2100 ? 1.5f : 0;      // a little jolt first
    allowBlink = false;
    return;
  }

  // ---- Poked ----
  if (now < giggleUntil) {
    tgt.happy = 0.85f; tgt.browRaise = 4; mouth = MOUTH_BIG_SMILE;
    bounceAmp = 3; jitter = 0.8f; allowBlink = false;
    return;
  }
  if (now < grumpyUntil) {
    float k = pokeLevel >= 3 ? 1.0f : 0.6f;
    tgt.angry = 0.45f * k; tgt.flat = 0.3f; tgt.browSlant = 0.9f * k; tgt.browRaise = -2;
    mouth = pokeLevel >= 3 ? MOUTH_FROWN : MOUTH_DASH;
    jitter = pokeLevel >= 3 ? 0.7f : 0;
    return;
  }

  // ---- Heard a clap or a knock: looks right at you ----
  if (now < noticeUntil) {
    tgt.lh = tgt.rh = BASE_H + 6; tgt.browRaise = 6; tgt.browR = 3;
    mouth = MOUTH_O;
    return;
  }

  // ---- Music: dances to the beat ----
  if (dancing) {
    notes = true;
    tgt.happy = 0.45f; tgt.browRaise = 3; mouth = MOUTH_SMILE;
    float ph = beatPeriodMs ? (float)((now - lastBeatTime) % beatPeriodMs) / beatPeriodMs : 0;
    danceBob = -fabsf(sinf(ph * PI)) * 6;
    tgt.tilt = ((now / beatPeriodMs) % 2 ? 5 : -5);
    allowBlink = false;
    return;
  }

  // ---- You are talking: it listens ----
  if (listening) {
    earWaves = true;
    tgt.lh = tgt.rh = BASE_H + 4; tgt.browRaise = 4; mouth = MOUTH_O;
    return;
  }

  // ---- Out for a walk ----
  if (walking) {
    tgt.happy = 0.35f; tgt.browRaise = 2; mouth = MOUTH_SMILE;
    float since = (now - lastStepTime) / 250.0f;
    danceBob = since < 1 ? -sinf(since * PI) * 4 : 0;     // little hop each step
    return;
  }

  // ---- Being rocked: eyelids get heavy ----
  if (rockDrowsy > 0.05f) {
    tgt.flat = 0.15f + rockDrowsy * 0.55f;
    tgt.browRaise = -3 * rockDrowsy;
    mouth = rockDrowsy > 0.6f ? MOUTH_NONE : MOUTH_DASH;
    if (rockDrowsy > 0.7f) tgt.lh = tgt.rh = BASE_H * (1.2f - rockDrowsy);
    return;
  }

  // ---- Battery running low: tired (it needs power, not WiFi) ----
  if (powerState == PWR_BATTERY && batteryShown >= 0 && batteryShown <= 15) {
    tgt.tired = batteryShown <= 6 ? 0.7f : 0.5f; tgt.flat = batteryShown <= 6 ? 0.45f : 0.25f;
    tgt.browRaise = -2; tgt.browSlant = -0.4f; mouth = MOUTH_DASH;
    return;
  }

  // ---- Moods ----
  switch (currentMood) {
    case MOOD_HAPPY:
      tgt.happy = 0.4f; tgt.browRaise = 2; mouth = MOUTH_SMILE;
      break;
    case MOOD_EXCITED:
      tgt.lw = tgt.rw = BASE_W + 4; tgt.lh = tgt.rh = BASE_H + 6;
      tgt.happy = 0.3f; tgt.browRaise = 5; mouth = MOUTH_BIG_SMILE;
      bounceAmp = 3; wanderFast = true;
      break;
    case MOOD_CURIOUS:
      if (wifiStats.openCount > 0) {
        tgt.tilt = 7; tgt.flat = 0.35f; tgt.browSlant = -0.2f; blush = true;
      } else {
        tgt.lh = BASE_H + 8; tgt.rh = BASE_H - 6; tgt.browL = 5;
        mouth = MOUTH_O; wanderFast = true;
      }
      break;
    case MOOD_BORED:
      tgt.flat = 0.5f; tgt.browRaise = -3; mouth = MOUTH_DASH;
      break;
    case MOOD_HUNGRY:
      tgt.tired = max(0.4f, L->baseTired); tgt.browSlant = -0.7f; mouth = MOUTH_O;
      break;
    case MOOD_SICK:
      eyeColor = EYE_SICK; tgt.tired = 0.5f;
      tgt.lh = tgt.rh = BASE_H - 8; tgt.lw = tgt.rw = BASE_W - 6;
      tgt.browSlant = -0.6f; mouth = MOUTH_WAVY; jitter = 1.2f;
      break;
    case MOOD_CALM:
    default:
      if (wifiStats.netCount > 15) {
        tgt.tilt = 6; tgt.lh = tgt.rh = BASE_H - 8; tgt.browSlant = 0.4f; tgt.browRaise = 2;
      }
      break;
  }

  // ---- Little idle quirks when nothing else is going on ----
  bool relaxed = currentMood == MOOD_CALM || currentMood == MOOD_HAPPY || currentMood == MOOD_CURIOUS;
  if (relaxed) {
    pickQuirk(now);
    switch (quirk) {
      case Q_WINK:
        if (winkSide < 0) tgt.lh = 3; else tgt.rh = 3;
        tgt.happy = max(tgt.happy, 0.3f); mouth = MOUTH_SMILE; allowBlink = false;
        break;
      case Q_YAWN:
        tgt.flat = 0.45f; tgt.browRaise = -2; mouth = MOUTH_YAWN; allowBlink = false;
        break;
      case Q_LOOK:
        tgt.happy = 0.35f; tgt.browRaise = 3; mouth = MOUTH_SMILE;
        break;
      default: break;
    }
  } else {
    quirk = Q_NONE;
  }
}

// ---------------------------------------------------------------------------
// Gaze & blink
// ---------------------------------------------------------------------------
void updateGaze(unsigned long now) {
  if (introRunning(now)) return;                 // the intro script sets the gaze
  if (dizzy) { gazeTx = 6 * sinf(now / 90.0f); gazeTy = 4 * cosf(now / 110.0f); return; }
  if (sweepGaze) { gazeTx = 10.0f * sinf(now / 300.0f); gazeTy = 0; return; }
  if (quirk == Q_LOOK || listening || startled || millis() < noticeUntil || millis() < calledUntil) { gazeTx = 0; gazeTy = 0; return; }

  if (currentMood == MOOD_BORED && currentActivity == ACT_NONE) {
    if (now >= nextGaze) {
      gazeTx = random(0, 2) ? 22 : -22; gazeTy = 6;
      nextGaze = now + random(3000, 6000);
    }
    return;
  }
  if (now >= nextGaze) {
    gazeTx = random(-20, 21);
    gazeTy = random(-7, 8);
    if (random(0, 3) == 0) { gazeTx = 0; gazeTy = 0; }
    nextGaze = now + (wanderFast ? random(500, 1400) : random(1500, 4000));
  }
}

float blinkFactor(unsigned long now) {
  if (!allowBlink) { blinking = false; return 1.0f; }
  if (!blinking && now >= nextBlink) { blinking = true; blinkStart = now; }
  if (!blinking) return 1.0f;
  float p = (now - blinkStart) / (float)BLINK_MS;
  if (p >= 1.0f) {
    blinking  = false;
    nextBlink = now + random(2000, 6000);
    if (random(0, 6) == 0) nextBlink = now + 250;
    return 1.0f;
  }
  return 1.0f - 0.9f * sinf(p * PI);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void drawEye(float cx, float cy, float w, float h, bool isLeft, bool closedLine, float pupilDx) {
  int iw = max(4, (int)w), ih = max(3, (int)h);
  int x = (int)(cx - iw / 2.0f), y = (int)(cy - ih / 2.0f);
  int r = min(sc(L->radius), min(iw, ih) / 2);
  const int M = HALO + 2;

  if (blush || L->cheeks)
    cv->fillEllipse((int)cx + (isLeft ? -sc(4) : sc(4)), y + ih + sc(4), (int)(iw * 0.30f), max(3, sc(6)),
                   L->cheeks && !blush ? rgb(90, 36, 60) : BLUSH);

  if (!closedLine) cv->fillRoundRect(x - HALO, y - HALO, iw + 2 * HALO, ih + 2 * HALO, r + HALO, L->halo);
  cv->fillRoundRect(x, y, iw, ih, r, eyeColor);

  // Baby: big dark pupils that follow the gaze, with white sparkles in them
  if (ih > 14 && L->sparkles > 0 && !dizzy && cur.pupil < 0.3f) {
    int pr = (int)(min(iw, ih) * 0.30f);
    int pcx = (int)cx + (int)constrain(cur.gx * 0.35f * S, -(float)(iw / 2 - pr - 3), (float)(iw / 2 - pr - 3));
    int pcy = (int)cy + (int)constrain(cur.gy * 0.35f * S, -(float)(ih / 2 - pr - 3), (float)(ih / 2 - pr - 3));
    cv->fillCircle(pcx, pcy, pr, rgb(34, 26, 48));
    cv->fillCircle(pcx - pr / 3, pcy - pr / 3, max(2, pr / 3), TEXT);
    if (L->sparkles == 2) cv->fillCircle(pcx + pr / 3, pcy + pr / 3, max(1, pr / 6), TEXT);
  }

  // Focus pupils (hunting)
  if (cur.pupil > 0.05f && ih > 10) {
    int ps = max(4, (int)(min(iw, ih) * 0.42f * cur.pupil));
    int px = (int)(cx + pupilDx * (iw / 2 - ps / 2 - 4)) - ps / 2;
    int py = (int)cy - ps / 2 + 1;
    cv->fillRoundRect(px, py, ps, ps, max(1, ps / 4), L->halo);
    cv->fillRect(px + 2, py + 2, max(2, ps / 4), max(2, ps / 4), eyeColor);
  }

  // Dizzy spiral
  if (dizzy && ih > 12) {
    float spin = millis() / 120.0f * (isLeft ? 1 : -1);
    float maxR = min(iw, ih) * 0.42f;
    float px = cx, py = cy;
    for (float a = 0; a < 3.2f * PI; a += 0.3f) {
      float rr = maxR * a / (3.2f * PI);
      float nx = cx + cosf(a + spin) * rr, ny = cy + sinf(a + spin) * rr;
      cv->drawWideLine((int)px, (int)py, (int)nx, (int)ny, 1.3f, L->halo);
      px = nx; py = ny;
    }
  }

  if (cur.tired > 0.02f) {
    int d = (int)(ih * cur.tired);
    if (isLeft) cv->fillTriangle(x - M, y - M, x + iw + M, y - M, x - M, y + d, BG);
    else        cv->fillTriangle(x - M, y - M, x + iw + M, y - M, x + iw + M, y + d, BG);
  }
  if (cur.angry > 0.02f) {
    int d = (int)(ih * cur.angry);
    if (isLeft) cv->fillTriangle(x - M, y - M, x + iw + M, y - M, x + iw + M, y + d, BG);
    else        cv->fillTriangle(x - M, y - M, x + iw + M, y - M, x - M, y + d, BG);
  }
  if (cur.flat > 0.02f) cv->fillRect(x - M, y - M, iw + 2 * M, (int)(ih * cur.flat) + M, BG);
  if (cur.happy > 0.02f) {
    int rx = (int)(iw * 0.78f), ry = (int)(ih * 0.6f);
    int ecy = (int)(y + ih + ry * 0.55f - cur.happy * ih * 0.65f);
    cv->fillEllipse((int)cx, ecy + HALO, rx + HALO, ry + HALO, BG);
  }

  // Elder's round glasses go on top
  if (L->glasses) {
    int gw = (int)(BASE_W * S) + sc(10), gh = (int)(BASE_H * S) + sc(12);
    int gx = (int)cx - gw / 2, gy = (int)cy - gh / 2;
    cv->drawRoundRect(gx, gy, gw, gh, gh / 2, GREY);
    cv->drawRoundRect(gx + 1, gy + 1, gw - 2, gh - 2, gh / 2 - 1, GREY);
  }
}

void drawBrow(float cx, float eyeTop, bool isLeft) {
  if (!L->brows) return;
  float extra = isLeft ? cur.browL : cur.browR;
  float y     = eyeTop - (9 + cur.browRaise + extra + (L->glasses ? 6 : 0)) * S;
  float half  = L->browHalf * S;
  float inner = isLeft ? cx + half - 4 * S : cx - half + 4 * S;
  float outer = isLeft ? cx - half - 2 * S : cx + half + 2 * S;
  float slope = cur.browSlant * 5 * S;
  cv->drawWideLine((int)outer, (int)(y - slope), (int)inner, (int)(y + slope), L->browThick, L->browColor);
}

void drawMouth(unsigned long now, float mx, float my, float tilt) {
  uint16_t c = TEXT;
  int x = (int)mx, y = (int)my;
  int t = (int)(tilt * 0.35f);
  int m = max(5, sc(8));

  switch (mouth) {
    case MOUTH_NONE: break;
    case MOUTH_DASH:      cv->drawWideLine(x - m, y + t, x + m, y - t, 1.4f, c); break;
    case MOUTH_SMILE:     cv->fillArc(x, y - sc(6), sc(6), sc(9), 25, 155, c); break;
    case MOUTH_BIG_SMILE: cv->fillArc(x, y - sc(8), sc(8), sc(12), 15, 165, c); break;
    case MOUTH_FROWN:     cv->fillArc(x, y + sc(8), sc(6), sc(9), 205, 335, c); break;
    case MOUTH_O:
      cv->fillEllipse(x, y, sc(5), sc(6), c);
      cv->fillEllipse(x, y, max(1, sc(2)), max(2, sc(3)), BG);
      break;
    case MOUTH_YAWN:
      cv->fillEllipse(x, y + sc(2), sc(8), sc(10), c);
      cv->fillEllipse(x, y + sc(2), sc(5), sc(7), BG);
      break;
    case MOUTH_WAVY:
      for (int i = -9; i < 9; i += 3)
        cv->drawWideLine(x + sc(i), y + ((i / 3) % 2 ? 2 : -1), x + sc(i + 3), y + ((i / 3) % 2 ? -1 : 2), 1.0f, c);
      break;
    case MOUTH_CHEW:
      if ((now / 160) % 2) { cv->fillEllipse(x, y, sc(6), sc(5), c); cv->fillEllipse(x, y, max(1, sc(3)), max(1, sc(2)), BG); }
      else                 { cv->drawWideLine(x - m, y, x + m, y, 1.4f, c); }
      break;
  }
}

void drawRadar(unsigned long now, int cx, int cy) {
  const int maxR = (int)(sqrtf((float)(W * W + H * H)) / 2) + 4;
  const uint32_t PERIOD = 1500;
  for (int i = 0; i < 3; i++) {
    float p = ((now + i * PERIOD / 3) % PERIOD) / (float)PERIOD;
    int r = 18 + (int)(p * (maxR - 18));
    uint16_t c = dim(SEC_WIFI, 0.55f * (1.0f - p));      // WiFi = blue
    cv->drawCircle(cx, cy, r, c);
    cv->drawCircle(cx, cy, r + 1, c);
  }
}

void drawWifiIcon(int x, int y, uint16_t c, int bars) {
  for (int i = 0; i < 3 && i < bars; i++) {
    int r = 4 + i * 4;
    for (int a = 220; a <= 320; a += (i == 0 ? 25 : 14)) {
      float rad = a * DEG_TO_RAD;
      cv->fillRect(x + (int)(cosf(rad) * r), y + (int)(sinf(rad) * r), 2, 2, c);
    }
  }
  cv->fillRect(x - 1, y - 1, 3, 3, c);
}

void drawLockIcon(int x, int y, uint16_t c) {
  cv->drawArc(x + 2, y + 5, 3, 4, 180, 360, c);
  cv->fillRect(x + 5, y + 4, 2, 2, c);
  cv->fillRoundRect(x - 3, y + 6, 11, 8, 1, c);
  cv->fillRect(x + 1, y + 9, 2, 3, BG);
}

// The pet "speaks" in its own colour (terracotta)
void drawBadge(const char* text) {
  cv->setTextSize(1);
  int tw = cv->textWidth(text);
  int w = tw + 12, h = 13;
  if (w > W - 4) w = W - 4;
  int x = CX - w / 2;
  cv->fillRoundRect(x, BADGE_Y, w, h, 3, BG);
  cv->drawRoundRect(x, BADGE_Y, w, h, 3, TERRA);
  cv->fillTriangle(CX - 3, BADGE_Y, CX + 3, BADGE_Y, CX, BADGE_Y - 4, TERRA);
  cv->setTextColor(TEXT);
  cv->setCursor(x + 6, BADGE_Y + 3);
  cv->print(text);
}

void drawHeart(int cx, int cy, int r, uint16_t c) {
  cv->fillCircle(cx - r, cy, r, c);
  cv->fillCircle(cx + r, cy, r, c);
  cv->fillTriangle(cx - 2 * r, cy + 1, cx + 2 * r, cy + 1, cx, cy + 2 * r + 2, c);
}

void drawStar(int x, int y, int r, uint16_t c) {
  cv->drawWideLine(x - r, y, x + r, y, 1.0f, c);
  cv->drawWideLine(x, y - r, x, y + r, 1.0f, c);
  cv->fillRect(x - 1, y - 1, 3, 3, c);
}

void drawBolt(int x, int y, int s, uint16_t c) {
  // a little lightning bolt, s = height
  cv->fillTriangle(x + s / 4, y, x - s / 4, y + s / 2 + 1, x + s / 8, y + s / 2 + 1, c);
  cv->fillTriangle(x - s / 8, y + s / 2 - 1, x + s / 4, y + s / 2 - 1, x - s / 4, y + s, c);
}

void drawOverlays(unsigned long now) {
  const bool asleepNow = currentActivity == ACT_REST && restPhase != REST_NONE;
  int iconY = portrait ? CY - 52 : 18;
  int iconL = portrait ? CX - 40 : CX - 66;
  int iconR = portrait ? CX + 34 : CX + 80;

  if (wifiScanInProgress)                       drawWifiIcon(iconL, iconY, SEC_WIFI, 1 + (now / 300) % 3);
  else if (wifiStats.netCount > 15 && currentActivity == ACT_NONE && restPhase == REST_NONE)
                                                drawWifiIcon(iconL, iconY, AMBER, 3);
  bool lockShown = wifiStats.openCount > 0 && lastWifiScanTime > 0 && restPhase == REST_NONE;
  if (lockShown)                                drawLockIcon(iconR, iconY, AMBER);

  // Charging: a bolt (it is eating electricity); right after plugging in, sparks fly
  if (powerState != PWR_BATTERY)
    drawBolt(lockShown ? iconR - 16 : iconR, iconY - 2, 12, powerState == PWR_FULL ? GOOD : C_HUNGER);
  if (now < zapUntil) {
    static const int8_t P[4][2] = { {-96,-24},{96,-20},{-88,26},{90,28} };
    for (int i = 0; i < 4; i++)
      if ((now / 120 + i) % 3) drawBolt(CX + sc(P[i][0]), CY + sc(P[i][1]) - 6, 12, C_HUNGER);
  }

  if (now < playEffectUntil) {
    int t = 1500 - (int)(playEffectUntil - now);
    int spread = portrait ? 46 : 84;
    for (int i = 0; i < 4; i++) {
      int phase = t - i * 200;
      if (phase < 0) continue;
      int x = (i % 2 ? CX + spread : CX - spread) + (int)(sinf(phase / 120.0f) * 5);
      int y = CY + 12 - phase / 18;
      if (y < 4) continue;
      drawHeart(x, y, portrait ? 3 : 4, HEART);
    }
  }

  // Evolution: twinkling stars in the new stage's colour
  if (sparkleBurst) {
    static const int8_t P[8][2] = { {-92,-30},{92,-26},{-80,24},{86,30},{-50,-44},{48,-46},{-104,0},{104,4} };
    uint16_t sc_ = petStage == STAGE_BABY ? ST_BABY : petStage == STAGE_TEEN ? ST_TEEN
                 : petStage == STAGE_ADULT ? ST_ADULT : ST_ELDER;
    for (int i = 0; i < 8; i++) {
      float ph = (now / 90 + i * 3) % 12 / 12.0f;
      int r = 2 + (int)(5 * sinf(ph * PI));
      drawStar(CX + sc(P[i][0]), CY + sc(P[i][1]), r, sc_);
    }
  }

  if (showZzz) {
    int dy = (now / 300) % 8;
    int zx = portrait ? CX + 22 : CX + 70;
    int zy = portrait ? CY - 30 : 34;
    cv->setTextColor(EYE_SLEEP);
    cv->setTextSize(1); cv->setCursor(zx, zy - dy);           cv->print("z");
    cv->setTextSize(2); cv->setCursor(zx + 10, zy - 16 - dy); cv->print("Z");
    cv->setTextSize(1);
  }

  // Speech badge
  char buf[28];
  bool scanning = wifiScanInProgress;
  if (scanning && !lastScanning) badgeUntil = 0;
  if (!scanning && lastScanning) badgeUntil = now + 5000;
  lastScanning = scanning;

  // Startled: a big "!" above
  if (startled) {
    int x = CX + sc(62), y = CY - sc(46);
    cv->fillRoundRect(x - 2, y, 5, sc(14), 2, AMBER);
    cv->fillRect(x - 2, y + sc(17), 5, 5, AMBER);
  }

  // Listening: sound waves on both sides, in your colour (it is YOUR voice)
  if (earWaves) {
    int ph = (now / 180) % 3;
    for (int i = 0; i <= ph; i++) {
      int r = sc(8 + i * 7);
      cv->drawArc(CX - sc(92), CY, r, r + 1, 140, 220, TEAL);
      cv->drawArc(CX + sc(92), CY, r, r + 1, 320, 360, TEAL);
      cv->drawArc(CX + sc(92), CY, r, r + 1, 0, 40, TEAL);
    }
  }

  // Dancing: music notes floating up
  if (notes) {
    for (int i = 0; i < 2; i++) {
      float ph = ((now + i * 700) % 1400) / 1400.0f;
      int x = (i ? CX + sc(92) : CX - sc(96)) + (int)(sinf(ph * 6.28f) * 4);
      int y = CY + sc(20) - (int)(ph * sc(50));
      cv->fillEllipse(x, y, 4, 3, C_HAPPY);
      cv->fillRect(x + 3, y - 11, 2, 11, C_HAPPY);
      cv->fillRect(x + 3, y - 11, 6, 2, C_HAPPY);
    }
  }

  // Too hot: a sweat drop next to the right eye
  if (runningHot && restPhase == REST_NONE) {
    int sx = CX + sc(86), sy = CY - sc(20) + (int)((now / 120) % 10);
    cv->fillCircle(sx, sy, max(2, sc(4)), SEC_WIFI);
    cv->fillTriangle(sx - max(2, sc(4)), sy - 1, sx + max(2, sc(4)), sy - 1, sx, sy - max(5, sc(9)), SEC_WIFI);
  }

  // Boot: our name fades in under the eyes once they have popped in
  if (introRunning(now)) {
    long t = (long)(now - introStart) - (long)introBlack;
    if (t > 600 && t < (long)INTRO_MS) {
      float a = min(1.0f, (t - 600) / 400.0f) * min(1.0f, ((long)INTRO_MS - t) / 300.0f);
      int sz = portrait ? 1 : 2;
      cv->setTextSize(sz);
      int w = cv->textWidth("sila");
      int x = CX - w / 2, y = BADGE_Y - (portrait ? 0 : 4);
      cv->setTextColor(dim(TEAL, a));   cv->setCursor(x, y); cv->print("si");
      cv->setTextColor(dim(TERRA, a)); cv->print("la");
      cv->setTextSize(1);
    }
    return;
  }
  if (now < evolveUntil) {
    static const char* names[4] = { "BABY", "TEEN", "ADULT", "ELDER" };
    snprintf(buf, sizeof(buf), portrait ? "%s!" : "I'm %s now!", names[constrain((int)petStage, 0, 3)]);
    drawBadge(buf);
  } else if (now < fallUntil) {
    drawBadge("AAAAH!");
  } else if (now < ouchUntil) {
    drawBadge("ow!");
  } else if (now < wheeUntil) {
    drawBadge("wheee!");
  } else if (startled) {
    drawBadge("!?");
  } else if (now < calledUntil) {
    drawBadge(portrait ? "yes?" : "you called?");
  } else if (now < zapUntil) {
    drawBadge(portrait ? "yum!" : "yum! power!");
  } else if (now < giggleUntil) {
    drawBadge("hehe!");
  } else if (now < grumpyUntil) {
    drawBadge(pokeLevel >= 3 ? (portrait ? "stop it!" : "stop poking!") : "hey!");
  } else if (now < stepGoalUntil) {
    drawBadge(portrait ? "GOAL!" : "step goal reached!");
  } else if (now < stirUntil && restPhase == REST_DEEP) {
    drawBadge("mm...");
  } else if (dizzy) {
    drawBadge("dizzy...");
  } else if (dancing && !asleepNow) {
    drawBadge(portrait ? "dancing" : "dancing!");
  } else if (listening && !asleepNow) {
    drawBadge(portrait ? "listening" : "listening...");
  } else if (walking && (now / 2500) % 2 == 0) {
    snprintf(buf, sizeof(buf), portrait ? "%lu" : "walkies! %lu", (unsigned long)stepsToday);
    drawBadge(buf);
  } else if (rockDrowsy > 0.4f) {
    drawBadge("zz...");
  } else if (now < peopleNewUntil) {
    snprintf(buf, sizeof(buf), portrait ? "%d near!" : "%d people near!", peopleNearby);
    drawBadge(buf);
  } else if (scanning) {
    drawBadge(currentActivity == ACT_HUNT ? "hunting..." : "sniffing...");
  } else if (now < badgeUntil) {
    if (wifiStats.openCount > 0 && (badgeUntil - now) < 2500)
      snprintf(buf, sizeof(buf), "%d open", wifiStats.openCount);
    else
      snprintf(buf, sizeof(buf), portrait ? "%d nets" : "%d networks", wifiStats.netCount);
    drawBadge(buf);
  } else if (petBedtime && restPhase == REST_DEEP && (now / 4000) % 3 == 0) {
    drawBadge(portrait ? "pick me up" : "pick me up to wake");
  } else if (quirk == Q_YAWN) {
    drawBadge("*yawn*");
  } else if (restPhase == REST_NONE && currentActivity == ACT_NONE) {
    bool show = (now / 3000) % 2 == 0;
    bool lowBatt = powerState == PWR_BATTERY && batteryShown >= 0 && batteryShown <= 15;
    if (show && lowBatt)                        drawBadge(batteryShown <= 6 ? (portrait ? "charge me!" : "feed me power!") : (portrait ? "low power" : "low battery..."));
    else if (show && currentMood == MOOD_HUNGRY) drawBadge("hungry");
    else if (show && currentMood == MOOD_SICK)  drawBadge(portrait ? "unwell" : "not feeling well");
    else if (show && currentMood == MOOD_BORED && wifiStats.netCount == 0) drawBadge("no WiFi here...");
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
void faceReset() {
  layout();
  setNeutral();
  cur = tgt;
  cur.gx = cur.gy = 0;
  inited    = true;
  lastFrame = millis();
  nextBlink = millis() + 1500;
  nextGaze  = millis() + 800;
  nextQuirk = millis() + random(12000, 25000);
  quirk     = Q_NONE;
}

void faceStartIntro(bool blackFirst) {
  faceReset();
  introStart = millis();
  if (introStart == 0) introStart = 1;
  introBlack = blackFirst ? 650 : 0;
}

bool faceIntroDone() {
  if (introStart == 0) return true;
  if (introRunning(millis())) return false;
  introStart = 0;
  return true;
}

// Size of the face during the intro: 0 while black, then an elastic pop
float faceIntroScale() {
  unsigned long now = millis();
  if (introStart == 0) return 1.0f;
  long t = (long)(now - introStart) - (long)introBlack;
  if (t < 0) return 0.0f;
  if (t >= (long)INTRO_POP_MS) return 1.0f;
  float p = t / (float)INTRO_POP_MS;
  const float c1 = 1.9f, c3 = c1 + 1;                   // ease-out-back (overshoot ~12%)
  return 1 + c3 * powf(p - 1, 3) + c1 * powf(p - 1, 2);
}

void faceRender() {
  unsigned long now = millis();
  if (!inited) faceReset();
  layout();

  if (introStart != 0 && (long)(now - introStart) < (long)introBlack) {
    cv->fillSprite(BG);
    return;
  }

  float dt = (float)(now - lastFrame);
  lastFrame = now;
  float k  = constrain(dt * 0.012f, 0.0f, 1.0f);
  float kg = constrain(dt * 0.009f, 0.0f, 1.0f);

  pickExpression(now);
  updateGaze(now);
  // Eyes lag behind when the stick is moved (floating feel)
  tgt.gx = gazeTx + constrain(-faceMotionX * 45.0f, -16.0f, 16.0f);
  tgt.gy = gazeTy + constrain(-faceMotionY * 35.0f, -8.0f, 8.0f);

  float* c = &cur.lw;
  float* t = &tgt.lw;
  const int N = sizeof(FaceParams) / sizeof(float);
  for (int i = 0; i < N; i++) c[i] = ease(c[i], t[i], (i == 4 || i == 5) ? kg : k);

  float blink = blinkFactor(now);
  // Gentle breathing: eyes swell a little, slower when asleep
  bool asleep = restPhase == REST_DEEP;
  float breath = 1.0f + (asleep ? 0.035f * sinf(now / 900.0f) : 0.018f * sinf(now / 650.0f));
  float bob    = bounceAmp > 0 ? -fabsf(sinf(now / 140.0f)) * bounceAmp : 0;
  float jx     = jitter > 0 ? random(-10, 11) / 10.0f * jitter : 0;

  float lookX = cur.pupil > 0.5f ? 0 : cur.gx;
  float dx = (lookX + jx) * S;
  float dy = (cur.gy + bob + danceBob) * S;
  float persp = lookX * 0.10f;
  float pupilDx = constrain(cur.gx / 10.0f, -1.0f, 1.0f);

  float off = (GAP / 2 + BASE_W / 2) * S;
  float lcx = CX - off + dx;
  float rcx = CX + off + dx;
  float lcy = CY + dy + cur.tilt * S;
  float rcy = CY + dy - cur.tilt * S;
  float lh  = cur.lh * blink * S * breath, rh = cur.rh * blink * S * breath;
  bool  closed = lh < 8 && rh < 8;

  cv->fillSprite(BG);
  if (radar) drawRadar(now, CX, CY);
  drawEye(lcx, lcy, (cur.lw - persp) * S, lh, true,  closed, pupilDx);
  drawEye(rcx, rcy, (cur.rw + persp) * S, rh, false, closed, pupilDx);
  if (L->glasses) cv->drawWideLine((int)(lcx + BASE_W * S / 2 + 4 * S), (int)lcy - 2, (int)(rcx - BASE_W * S / 2 - 4 * S), (int)rcy - 2, 1.2f, GREY);
  drawBrow(lcx, lcy - cur.lh * S / 2, true);
  drawBrow(rcx, rcy - cur.rh * S / 2, false);
  drawMouth(now, CX + dx * 0.4f, MOUTH_Y + dy * 0.4f, cur.tilt * S);
  drawOverlays(now);
}

void faceRenderDead() {
  layout();
  cv->fillSprite(BG);
  float off = (GAP / 2 + BASE_W / 2) * S;
  int a = sc(13);
  for (float ex : { CX - off, CX + off }) {
    cv->drawWideLine((int)ex - a, CY - a, (int)ex + a, CY + a, 2.5f, GREY);
    cv->drawWideLine((int)ex - a, CY + a, (int)ex + a, CY - a, 2.5f, GREY);
  }
  cv->fillArc(CX, MOUTH_Y + sc(8), sc(6), sc(9), 205, 335, GREY);
}
