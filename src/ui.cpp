// Sila - all screens.
//
// Colour language (see include/theme.h): teal = you, terracotta = me (the
// pet's voice), and every section / stat has its own meaningful colour.
// Everything draws into the full-screen framebuffer `fb`, pushed once per frame.

#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#include "ui.h"
#include "ui_anim.h"
#include "theme.h"

using namespace theme;

// ---------------- Layout ----------------
static const int HDR_H    = 21;
static const int PANEL_X  = 3;
static const int PANEL_Y  = 24;
static const int PANEL_W  = SCR_W - 6;
static const int PANEL_H  = 98;
static const int FOOTER_Y = SCR_H - 10;
static const int ROW_TOP  = PANEL_Y;
static const int ROW_STEP = 14;

// ---------------- Local UI state ----------------
static int hatchFrameUi = 0;   static unsigned long lastHatchFrameUi = 0;
static unsigned long hatchScreenSince = 0;

static Screen lastScreen = SCREEN_BOOT;

// Floating face physics
static float faceAngle = 0, angVel = 0, lastTargetAngle = 0;
static float offX = 0, offY = 0, offVX = 0, offVY = 0;
static unsigned long lastPhys = 0;

// List selector + menu carousel
static int   menuY = ROW_TOP, menuTargetY = ROW_TOP;
static unsigned long lastMenuAnim = 0;
static float carouselPos = 0;
static unsigned long lastCarousel = 0;

static int  batteryPct    = -1;
static bool batteryCharge = false;

// ===========================================================================
// Text helpers
// ===========================================================================
static const char* moodText(Mood m) {
  switch (m) {
    case MOOD_HUNGRY:  return "HUNGRY";
    case MOOD_HAPPY:   return "HAPPY";
    case MOOD_CURIOUS: return "CURIOUS";
    case MOOD_BORED:   return "BORED";
    case MOOD_SICK:    return "SICK";
    case MOOD_EXCITED: return "EXCITED";
    case MOOD_CALM:    return "CALM";
  }
  return "?";
}

static const char* stageText(Stage s) {
  switch (s) {
    case STAGE_BABY:  return "BABY";
    case STAGE_TEEN:  return "TEEN";
    case STAGE_ADULT: return "ADULT";
    case STAGE_ELDER: return "ELDER";
  }
  return "?";
}

static uint16_t stageColor(Stage s) {
  switch (s) {
    case STAGE_BABY:  return ST_BABY;
    case STAGE_TEEN:  return ST_TEEN;
    case STAGE_ADULT: return ST_ADULT;
    default:          return ST_ELDER;
  }
}

static uint16_t moodColor(Mood m) {
  switch (m) {
    case MOOD_HUNGRY:  return C_HUNGER;
    case MOOD_HAPPY:
    case MOOD_EXCITED: return C_HAPPY;
    case MOOD_SICK:    return ALERT;
    case MOOD_CURIOUS: return SEC_WIFI;
    case MOOD_BORED:   return GREY;
    default:           return TEAL;
  }
}

static const char* activityText() {
  if (currentActivity == ACT_REST) return petBedtime ? "in bed" : rockedSleep ? "rocked asleep" : "napping";
  switch (currentActivity) {
    case ACT_HUNT:     return "hunting";
    case ACT_DISCOVER: return "exploring";
    default:           return "idle";
  }
}

// ===========================================================================
// Primitives
// ===========================================================================
// Power state comes from main.cpp (smoothed, no flicker)
static void updateBattery() {
  batteryPct    = batteryShown;
  batteryCharge = powerState == PWR_CHARGING;
}

static void drawBatteryIcon(int x, int y) {
  updateBattery();
  fb.drawRect(x, y, 14, 7, GREY);
  fb.fillRect(x + 14, y + 2, 2, 3, GREY);
  if (batteryPct < 0) return;
  if (powerState == PWR_CHARGING) {
    // filling animation in "your" teal
    int lvl = max(1, 10 * batteryPct / 100);
    int anim = lvl + (int)((millis() / 300) % (11 - min(lvl, 10)));
    fb.fillRect(x + 2, y + 2, min(10, anim), 3, TEAL);
  } else if (powerState == PWR_FULL) {
    fb.fillRect(x + 2, y + 2, 10, 3, GOOD);
  } else {
    uint16_t c = batteryPct > 50 ? GOOD : batteryPct > 20 ? AMBER : ALERT;
    fb.fillRect(x + 2, y + 2, max(1, 10 * batteryPct / 100), 3, c);
  }
}


static void textAt(int x, int y, const char* s, uint16_t c, int size = 1) {
  fb.setTextSize(size); fb.setTextColor(c); fb.setCursor(x, y); fb.print(s); fb.setTextSize(1);
}
static void textRight(int xr, int y, const char* s, uint16_t c, int size = 1) {
  fb.setTextSize(size); textAt(xr - fb.textWidth(s), y, s, c, size);
}
static void textCenter(int y, const char* s, uint16_t c, int size = 1, int cx = SCR_W / 2) {
  fb.setTextSize(size); textAt(cx - fb.textWidth(s) / 2, y, s, c, size);
}

// The name, built from both of us: "si" in teal (you) + "la" in terracotta (me)
static void drawWordmark(int x, int y, int size = 1) {
  fb.setTextSize(size);
  int w = fb.textWidth("si");
  textAt(x, y, "si", TEAL, size);
  textAt(x + w, y, "la", TERRA, size);
}

// Header in the section's colour
static void header(const char* title, uint16_t col, const char* status = nullptr, uint16_t statusCol = TEXT) {
  fb.fillRect(0, 0, SCR_W, HDR_H, BG);
  fb.fillRoundRect(4, 4, 11, 11, 2, col);
  fb.fillRect(7, 7, 5, 5, BG);
  textAt(20, 3, title, TEXT, 2);
  drawBatteryIcon(SCR_W - 20, 3);
  if (status) textRight(SCR_W - 24, 3, status, statusCol);
  fb.drawFastHLine(0, 19, SCR_W, dim(col, 0.45f));
  fb.fillRect(4, 19, 60, 2, col);
}

static void panel(uint16_t col) {
  fb.fillRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL);
  fb.fillRect(PANEL_X, PANEL_Y, 2, PANEL_H, col);
}

// Hints are the pet talking -> the pet's colour
static void footer(const char* hint) { textAt(4, FOOTER_Y + 1, hint, TERRA); }

static void segBar(int x, int y, int w, int h, int value, uint16_t on) {
  const int segW = 4, gap = 2, slant = 2;
  int n = (w - slant) / (segW + gap);
  int filled = (n * constrain(value, 0, 100) + 50) / 100;
  for (int i = 0; i < n; i++) {
    int sx = x + i * (segW + gap);
    uint16_t c = i < filled ? on : SEG_OFF;
    fb.fillTriangle(sx + slant, y, sx + slant + segW, y, sx, y + h, c);
    fb.fillTriangle(sx + slant + segW, y, sx + segW, y + h, sx, y + h, c);
  }
}

static uint16_t statusColor(int v) { return v >= 60 ? GOOD : v >= 25 ? AMBER : ALERT; }

// ===========================================================================
// Lists (Controls / Settings)
// ===========================================================================
static int rowY(int i) { return ROW_TOP + i * ROW_STEP; }

static void animateSelector() {
  unsigned long now = millis();
  if (now - lastMenuAnim < MENU_ANIM_INTERVAL) return;
  lastMenuAnim = now;
  int diff = menuTargetY - menuY;
  if (abs(diff) <= MENU_ANIM_STEP) menuY = menuTargetY;
  else menuY += (diff > 0) ? MENU_ANIM_STEP : -MENU_ANIM_STEP;
}

static const int LIST_ROWS = 7;
static int listFirst(int count, int sel) { return count <= LIST_ROWS ? 0 : constrain(sel - 3, 0, count - LIST_ROWS); }

static void menuList(const char* const* labels, const char* const* values, int count, int sel, uint16_t col) {
  panel(col);
  animateSelector();
  int first = listFirst(count, sel);
  fb.fillRect(PANEL_X + 2, menuY, PANEL_W - 2, ROW_STEP, col);
  char num[4];
  // scroll hints
  if (first > 0)                      fb.fillTriangle(SCR_W - 6, PANEL_Y + 2, SCR_W - 10, PANEL_Y + 6, SCR_W - 2, PANEL_Y + 6, col);
  if (first + LIST_ROWS < count)      fb.fillTriangle(SCR_W - 6, PANEL_Y + PANEL_H - 2, SCR_W - 10, PANEL_Y + PANEL_H - 6, SCR_W - 2, PANEL_Y + PANEL_H - 6, col);
  for (int i = first; i < count && i < first + LIST_ROWS; i++) {
    int y = rowY(i - first) + 3;
    bool on = (i == sel) && abs(menuY - rowY(i - first)) < ROW_STEP / 2;
    snprintf(num, sizeof(num), "%02d", i + 1);
    textAt(10, y, num, on ? BG : col);
    textAt(28, y, labels[i], on ? BG : TEXT);
    if (values && values[i]) textRight(SCR_W - 14, y, values[i], on ? BG : col);
  }
}

// ===========================================================================
// FACE: floating-bottle physics
// The face is drawn on its own canvas and rotated onto the screen. Its angle
// follows gravity continuously (any angle, not just 90 degree steps) through a
// soft spring that slightly overshoots, like something floating in water.
// Moving the stick pushes the face around a little (inertia) and it settles.
// ===========================================================================
static void ensureFaceCanvas(bool portrait) {
  faceCanvas = portrait && facePort.width() > 0 ? &facePort : &faceLand;
}

static void stepFacePhysics() {
  unsigned long now = millis();
  float dt = lastPhys ? (now - lastPhys) / 1000.0f : 0.016f;
  lastPhys = now;
  dt = constrain(dt, 0.001f, 0.05f);

  bool faceMode = (currentScreen == SCREEN_HOME || currentScreen == SCREEN_BOOT) && autoRotate;
  float target = faceMode ? (gravValid ? gravAngle : lastTargetAngle) : 0.0f;
  lastTargetAngle = target;

  // Angle: underdamped spring (zeta ~0.55) -> floaty, small overshoot
  const float K = 55.0f, D = 8.2f;
  float diff = fmodf(target - faceAngle + 540.0f, 360.0f) - 180.0f;
  angVel += (diff * K - angVel * D) * dt;
  faceAngle += angVel * dt;
  faceAngle = fmodf(faceAngle + 360.0f, 360.0f);

  // Position: inertia from movement, spring back to the centre
  float tX = constrain(-motionX * 26.0f, -14.0f, 14.0f);
  float tY = constrain(-motionY * 20.0f, -10.0f, 10.0f);
  const float K2 = 38.0f, D2 = 6.5f;
  offVX += ((tX - offX) * K2 - offVX * D2) * dt;  offX += offVX * dt;
  offVY += ((tY - offY) * K2 - offVY * D2) * dt;  offY += offVY * dt;

  // Motion seen from the face's own frame (for the eyes' lag)
  float a = -faceAngle * DEG_TO_RAD;
  faceMotionX = motionX * cosf(a) - motionY * sinf(a);
  faceMotionY = motionX * sinf(a) + motionY * cosf(a);

  int quarter = ((int)lroundf(faceAngle / 90.0f)) % 4;
  ensureFaceCanvas(quarter % 2 == 1);
}

static void composeFace(float extraScale = 1.0f) {
  fb.fillSprite(BG);
  if (extraScale < 0.02f) return;
  float w = faceCanvas->width(), h = faceCanvas->height();
  float a = faceAngle * DEG_TO_RAD, c = fabsf(cosf(a)), sn = fabsf(sinf(a));
  float bw = w * c + h * sn, bh = w * sn + h * c;
  float z = min(1.0f, 1.5f * min(SCR_W / bw, SCR_H / bh)) * extraScale;
  faceCanvas->pushRotateZoom(&fb, SCR_W / 2.0f + offX, SCR_H / 2.0f + offY, faceAngle, z, z);
}

static void drawFaceScreen() {
  stepFacePhysics();
  faceRender();
  composeFace(faceIntroScale());
  updateBattery();
  if (batteryPct >= 0 && (batteryPct < 20 || powerState != PWR_BATTERY)) drawBatteryIcon(SCR_W - 20, 4);
  fb.pushSprite(0, 0);
}

// ===========================================================================
// BOOT: black, then the eyes pop in, blink and look around
// ===========================================================================
static void screenBoot() {
  drawFaceScreen();
  if (faceIntroDone()) {
    currentScreen = SCREEN_HOME;          // face keeps going - no reset, no jump
    uiOnScreenChange(currentScreen);
  }
}

// ===========================================================================
// HATCH: just the egg. Press A or shake it to hatch.
// ===========================================================================
static void drawEgg(int cx, int cy, int stage, unsigned long now) {
  const int rx = 24, ry = 31;
  int wob;
  if (stage == 0) {
    // Mostly still, with an occasional wiggle as if something moves inside
    unsigned long ph = now % 3200;
    wob = ph < 500 ? (int)(sinf(ph / 40.0f) * 3.0f * (1.0f - ph / 500.0f)) : 0;
  } else {
    wob = (int)(sinf(now / 40.0f) * (1 + stage));
  }
  int x = cx + wob;

  if (stage < 4) {
    fb.fillEllipse(x, cy, rx + 4, ry + 4, dim(ST_BABY, 0.18f));
    fb.fillEllipse(x, cy, rx, ry, TEXT);
    fb.fillCircle(x - 9, cy - 11, 5, ST_BABY);
    fb.fillCircle(x + 10, cy + 3, 6, TEAL);
    fb.fillCircle(x - 4, cy + 16, 4, TERRA);
    fb.fillRect(x - 12, cy - 22, 4, 4, EYE);           // shine
  } else {
    fb.fillEllipse(x, cy, rx, ry, TEXT);
    fb.fillRect(x - rx - 1, cy - ry - 1, 2 * rx + 3, ry - 2, BG);
    fb.fillEllipse(x, cy - ry - 10, rx - 2, 12, TEXT);
    fb.fillRect(x - rx, cy - ry + 3, 2 * rx, 8, BG);
    fb.fillRoundRect(x - 13, cy - 6, 10, 9, 4, EYE);
    fb.fillRoundRect(x + 3, cy - 6, 10, 9, 4, EYE);
  }
  static const int8_t crack[][2] = { { -22, -4 }, { -13, 3 }, { -5, -5 }, { 3, 3 }, { 11, -4 }, { 20, 2 } };
  int segs = (stage >= 4) ? 0 : min(5, stage * 2);
  for (int i = 0; i < segs; i++)
    fb.drawWideLine(x + crack[i][0], cy - 4 + crack[i][1], x + crack[i + 1][0], cy - 4 + crack[i + 1][1], 1.2f, BG);
}

static void screenHatch() {
  unsigned long now = millis();
  fb.fillSprite(BG);

  if (!hasHatchedOnce && !hatchTriggered) {
    drawEgg(SCR_W / 2, SCR_H / 2, 0, now);
    // Only after a while, a tiny hint in the pet's colour
    if (now - hatchScreenSince > 6000 && (now / 900) % 2)
      textCenter(SCR_H - 12, "shake me  /  A", dim(TERRA, 0.8f));
    fb.pushSprite(0, 0);
    return;
  }

  if (!hasHatchedOnce && hatchTriggered) {
    if (hatchFrameUi == 0 && now - lastHatchFrameUi >= HATCH_DELAY) sndHatch();
    if (now - lastHatchFrameUi >= HATCH_DELAY) {
      lastHatchFrameUi = now;
      if (hatchFrameUi < 5) hatchFrameUi++;
      else {
        hasHatchedOnce = true;
        hatchTriggered = false;
        hatchFrameUi   = 0;
        currentScreen  = SCREEN_HOME;
        uiOnScreenChange(currentScreen);
        faceStartIntro(false);            // eyes pop out of the shell
        return;
      }
    }
    drawEgg(SCR_W / 2, SCR_H / 2, min(hatchFrameUi, 4), now);
    fb.pushSprite(0, 0);
    return;
  }

  currentScreen = SCREEN_HOME;
  uiOnScreenChange(currentScreen);
}

// ===========================================================================
// HOME
// ===========================================================================
static void screenHome() { drawFaceScreen(); }

// ===========================================================================
// MENU: colourful card carousel
// ===========================================================================
struct MenuCard { const char* title; const char* line; uint16_t col; };
static const MenuCard CARDS[MAIN_MENU_COUNT] = {
  { "VITALS",   "how it feels",    SEC_VITALS   },
  { "PLAY",     "tilt & catch",    SEC_PLAY     },
  { "WALKS",    "steps together",  SEC_WALK     },
  { "PET",      "who it is",       SEC_PET      },
  { "RADAR",    "WiFi & people",   SEC_WIFI     },
  { "CONTROLS", "your buttons",    SEC_CONTROLS },
  { "SETTINGS", "tune it",         SEC_SETTINGS },
  { "SYSTEM",   "under the hood",  SEC_SYSTEM   },
  { "HOME",     "back to me",      SEC_HOME     },
};

static void iconHeart(int x, int y, int s, uint16_t c) {
  int r = s / 4;
  fb.fillCircle(x - r, y - r / 2, r, c);
  fb.fillCircle(x + r, y - r / 2, r, c);
  fb.fillTriangle(x - 2 * r, y - r / 4, x + 2 * r, y - r / 4, x, y + s / 2, c);
  // heartbeat line across
  fb.drawWideLine(x - s / 2 - 3, y, x - 4, y, 1.0f, BG);
  fb.drawWideLine(x - 4, y, x - 1, y - 5, 1.0f, BG);
  fb.drawWideLine(x - 1, y - 5, x + 2, y + 4, 1.0f, BG);
  fb.drawWideLine(x + 2, y + 4, x + 5, y, 1.0f, BG);
  fb.drawWideLine(x + 5, y, x + s / 2 + 3, y, 1.0f, BG);
}
static void iconFace(int x, int y, int s, uint16_t c) {
  fb.fillRoundRect(x - s / 2, y - s / 2 + 2, s, s - 4, 6, c);
  fb.fillRoundRect(x - s / 3, y - 4, s / 4, s / 5, 2, BG);
  fb.fillRoundRect(x + s / 12, y - 4, s / 4, s / 5, 2, BG);
  fb.fillRect(x - 3, y + s / 5, 6, 2, BG);
}
static void iconWifi(int x, int y, int s, uint16_t c) {
  for (int i = 0; i < 3; i++) fb.fillArc(x, y + s / 3, 5 + i * 6, 7 + i * 6, 225, 315, c);
  fb.fillCircle(x, y + s / 3, 3, c);
}
static void iconSliders(int x, int y, int s, uint16_t c) {
  const int kx[3] = { -6, 7, -1 };
  for (int i = 0; i < 3; i++) {
    int yy = y - s / 3 + i * (s / 3);
    fb.fillRoundRect(x - s / 2, yy - 1, s, 3, 1, dim(c, 0.6f));
    fb.fillCircle(x + kx[i], yy, 4, c);
  }
}
static void iconGear(int x, int y, int s, uint16_t c) {
  int r = s / 3;
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    fb.fillCircle(x + (int)(cosf(a) * (r + 2)), y + (int)(sinf(a) * (r + 2)), 3, c);
  }
  fb.fillCircle(x, y, r, c);
  fb.fillCircle(x, y, r / 2, BG);
}
static void iconChip(int x, int y, int s, uint16_t c) {
  int h = s / 2 - 3;
  for (int i = -1; i <= 1; i++) {
    fb.fillRect(x + i * 7 - 1, y - h - 4, 3, 4, c); fb.fillRect(x + i * 7 - 1, y + h + 1, 3, 4, c);
    fb.fillRect(x - h - 4, y + i * 7 - 1, 4, 3, c); fb.fillRect(x + h + 1, y + i * 7 - 1, 4, 3, c);
  }
  fb.fillRoundRect(x - h, y - h, 2 * h + 1, 2 * h + 1, 3, c);
  fb.fillRect(x - 4, y - 4, 9, 9, BG);
}
static void iconHome(int x, int y, int s, uint16_t c) {
  fb.fillTriangle(x - s / 2, y - 1, x + s / 2, y - 1, x, y - s / 2, c);
  fb.fillRect(x - s / 3, y - 1, 2 * s / 3, s / 2, c);
  fb.fillRoundRect(x - 3, y + 4, 7, s / 2 - 5, 1, BG);
}
static void iconSteps(int x, int y, int s, uint16_t c) {
  // two footprints: a sole and three toes each, one a step ahead of the other
  for (int f = 0; f < 2; f++) {
    int fx = x + (f ? s / 5 : -s / 5), fy = y + (f ? -s / 6 : s / 8);
    fb.fillEllipse(fx, fy + 3, s / 9, s / 6, c);
    for (int t = -1; t <= 1; t++) fb.fillCircle(fx + t * (s / 10), fy - s / 6 - (t == 0 ? 2 : 0), 2, c);
  }
}
static void iconCatch(int x, int y, int s, uint16_t c) {
  // a falling star above a little catching bowl
  int r = s / 6;
  fb.fillCircle(x, y - s / 4, r, c);
  fb.drawWideLine(x - 2, y - s / 4 - r - 5, x - 1, y - s / 4 - r - 1, 1.0f, c);
  fb.fillArc(x, y + 2, s / 3, s / 3 + 3, 20, 160, c);
}
static void drawMenuIcon(int i, int x, int y, int s, uint16_t c) {
  switch (i) {
    case 0: iconHeart(x, y, s, c);   break;
    case 1: iconCatch(x, y, s, c);   break;
    case 2: iconSteps(x, y, s, c);   break;
    case 3: iconFace(x, y, s, c);    break;
    case 4: iconWifi(x, y, s, c);    break;
    case 5: iconSliders(x, y, s, c); break;
    case 6: iconGear(x, y, s, c);    break;
    case 7: iconChip(x, y, s, c);    break;
    default: iconHome(x, y, s, c);   break;
  }
}

static void drawCard(int i, float d) {
  float ad = fabsf(d);
  if (ad > 1.6f) return;
  float k = 1.0f - 0.25f * min(ad, 1.0f);           // neighbours are smaller...
  float bright = 1.0f - 0.55f * min(ad, 1.0f);      // ...and dimmer
  int w = (int)(122 * k), h = (int)(84 * k);
  int cx = SCR_W / 2 + (int)(d * 116);
  int y = 18 + (84 - h) / 2;
  int x = cx - w / 2;
  uint16_t col = dim(CARDS[i].col, bright);

  fb.fillRoundRect(x, y, w, h, 8, dim(CARDS[i].col, 0.16f * bright));
  fb.drawRoundRect(x, y, w, h, 8, col);
  fb.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, dim(CARDS[i].col, 0.5f * bright));

  int iconS = (int)(30 * k);
  drawMenuIcon(i, cx, y + 8 + iconS / 2, iconS, col);
  if (ad < 0.5f) {
    textCenter(y + h - 33, CARDS[i].title, TEXT, 2, cx);
    textCenter(y + h - 13, CARDS[i].line, dim(CARDS[i].col, 0.9f), 1, cx);
  } else {
    textCenter(y + h - 16, CARDS[i].title, col, 1, cx);
  }
}

static void screenMenu(int idx) {
  unsigned long now = millis();
  float dt = lastCarousel ? (now - lastCarousel) : 16.0f;
  lastCarousel = now;
  float diff = idx - carouselPos;
  if (diff > MAIN_MENU_COUNT / 2.0f)  carouselPos += MAIN_MENU_COUNT;
  if (diff < -MAIN_MENU_COUNT / 2.0f) carouselPos -= MAIN_MENU_COUNT;
  carouselPos += (idx - carouselPos) * constrain(dt * 0.014f, 0.0f, 1.0f);
  if (fabsf(idx - carouselPos) < 0.002f) carouselPos = idx;

  fb.fillSprite(BG);
  // Top line: where you are + battery
  char pos[8];
  snprintf(pos, sizeof(pos), "%d/%d", idx + 1, MAIN_MENU_COUNT);
  drawWordmark(5, 4);
  textAt(32, 4, pos, GREY);
  drawBatteryIcon(SCR_W - 20, 3);

  // Neighbours first, then the centre card on top
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < MAIN_MENU_COUNT; i++) {
      float d = i - carouselPos;
      while (d >  MAIN_MENU_COUNT / 2.0f) d -= MAIN_MENU_COUNT;
      while (d < -MAIN_MENU_COUNT / 2.0f) d += MAIN_MENU_COUNT;
      bool centre = fabsf(d) < 0.5f;
      if ((pass == 0) != centre) drawCard(i, d);
    }

  // Dots in each section's colour
  const int dy = 110, gap = 12, x0 = SCR_W / 2 - (MAIN_MENU_COUNT - 1) * gap / 2;
  for (int i = 0; i < MAIN_MENU_COUNT; i++) {
    if (i == idx) { fb.fillCircle(x0 + i * gap, dy, 4, CARDS[i].col); }
    else          { fb.fillCircle(x0 + i * gap, dy, 2, dim(CARDS[i].col, 0.6f)); }
  }
  footer("B next   A open   hold B back");
  fb.pushSprite(0, 0);
}

// ===========================================================================
// VITALS: each stat in its own colour, a status dot and a plain hint
// ===========================================================================
static const char* hungerHint() {
  if (pet.hunger >= 70) return "Full. Eats WiFi on its own.";
  if (pet.hunger >= 50) return "Fine. Will hunt WiFi soon.";
  if (pet.hunger >= 25) return "Peckish - hunting WiFi.";
  return "Starving! Take it near WiFi.";
}
static const char* happyHint() {
  if (pet.happiness >= 70) return "Loves new & hidden networks.";
  if (pet.happiness >= 40) return "Poke it, pat it (hold A), PLAY.";
  return "Sad. Play Catch, find WiFi.";
}
static const char* healthHint() {
  if (pet.health >= 70) return "Healthy and strong.";
  if (pet.health >= 40) return "Lay it down to sleep & heal.";
  return "Weak! Feed it, then let it sleep.";
}

static void screenVitals() {
  fb.fillSprite(BG);
  header("VITALS", SEC_VITALS, moodText(currentMood), moodColor(currentMood));
  panel(SEC_VITALS);

  const char*    lab[3]  = { "HUNGER", "HAPPY", "HEALTH" };
  const int      val[3]  = { pet.hunger, pet.happiness, pet.health };
  const uint16_t col[3]  = { C_HUNGER, C_HAPPY, C_HEALTH };
  const char*    hint[3] = { hungerHint(), happyHint(), healthHint() };
  char pct[6];

  for (int i = 0; i < 3; i++) {
    int y = PANEL_Y + 3 + i * 32;
    textAt(9, y, lab[i], col[i]);
    snprintf(pct, sizeof(pct), "%d%%", val[i]);
    textRight(SCR_W - 18, y, pct, TEXT);
    fb.fillCircle(SCR_W - 11, y + 3, 3, statusColor(val[i]));
    segBar(9, y + 10, SCR_W - 18, 7, val[i], col[i]);
    textAt(9, y + 20, hint[i], GREY);
  }
  footer("B next page   A home");
  fb.pushSprite(0, 0);
}

static void kv(int x, int y, const char* k, const char* v, uint16_t kc, uint16_t vc = TEXT);
// ===========================================================================
// WALKS (lime = movement)
// ===========================================================================
static void screenWalks() {
  fb.fillSprite(BG);
  header("WALKS", SEC_WALK, walking ? "walking!" : "today", SEC_WALK);
  panel(SEC_WALK);
  const uint32_t GOAL = 3000;
  char v[24];

  // Big number
  snprintf(v, sizeof(v), "%lu", (unsigned long)stepsToday);
  textAt(10, PANEL_Y + 6, v, TEXT, 3);
  fb.setTextSize(3); int w = fb.textWidth(v); fb.setTextSize(1);
  textAt(14 + w, PANEL_Y + 19, "steps", SEC_WALK);

  // Goal bar
  int pct = (int)min<uint32_t>(100, stepsToday * 100 / GOAL);
  segBar(9, PANEL_Y + 36, SCR_W - 18, 8, pct, stepsToday >= GOAL ? GOOD : SEC_WALK);
  snprintf(v, sizeof(v), "%d%% of %lu", pct, (unsigned long)GOAL);
  textRight(SCR_W - 9, PANEL_Y + 8, v, GREY);

  snprintf(v, sizeof(v), "%lu", (unsigned long)stepsBest);
  kv(9, PANEL_Y + 52, "Best", v, SEC_WALK);
  snprintf(v, sizeof(v), "%lu", (unsigned long)stepsTotal);
  kv(9, PANEL_Y + 65, "Total", v, SEC_WALK);
  textAt(9, PANEL_Y + 82, stepsToday >= GOAL ? "Goal done! It loves walks." : "Every 100 steps: happier & healthier", GREY);
  footer("B next page   A home");
  fb.pushSprite(0, 0);
}

// ===========================================================================
// PET INFO (violet = identity)
// ===========================================================================
static void kv(int x, int y, const char* k, const char* v, uint16_t kc, uint16_t vc) {
  textAt(x, y, k, kc);
  textAt(x + 44, y, v, vc);
}

static void screenPetStatus() {
  fb.fillSprite(BG);
  header("PET", SEC_PET, stageText(petStage), stageColor(petStage));
  panel(SEC_PET);

  char age[20];
  snprintf(age, sizeof(age), "%lud %02luh %02lum", pet.ageDays, pet.ageHours, pet.ageMinutes);
  int y = PANEL_Y + 6;
  kv(9, y,      "Stage", stageText(petStage), SEC_PET, stageColor(petStage));
  kv(9, y + 14, "Age",   age, SEC_PET);
  kv(9, y + 28, "Mood",  moodText(currentMood), SEC_PET, moodColor(currentMood));
  kv(9, y + 42, "Now",   activityText(), SEC_PET);
  const char* next = petStage == STAGE_BABY ? "TEEN @20m" : petStage == STAGE_TEEN ? "ADULT @1h"
                   : petStage == STAGE_ADULT ? "ELDER @3h" : "max";
  Stage ns = petStage == STAGE_ELDER ? STAGE_ELDER : (Stage)(petStage + 1);
  kv(9, y + 56, "Next", next, SEC_PET, dim(stageColor(ns), 0.8f));

  // Stage path: four dots, lit up to the current stage
  for (int i = 0; i < 4; i++) {
    int x = 13 + i * 26;
    uint16_t c = stageColor((Stage)i);
    if (i <= (int)petStage) fb.fillCircle(x, y + 76, 4, c); else fb.drawCircle(x, y + 76, 4, dim(c, 0.6f));
    if (i < 3) fb.drawFastHLine(x + 6, y + 76, 14, i < (int)petStage ? c : SEG_OFF);
  }

  const int rx = 134;
  fb.drawFastVLine(rx - 6, PANEL_Y + 4, PANEL_H - 8, SEG_OFF);
  textAt(rx, y, "PERSONALITY", SEC_PET);
  const char* tl[3] = { "Curious", "Active", "Stress" };
  const int   tv[3] = { traitCuriosity, traitActivity, traitStress };
  for (int i = 0; i < 3; i++) {
    textAt(rx, y + 16 + i * 22, tl[i], TEXT);
    segBar(rx, y + 25 + i * 22, SCR_W - rx - 10, 5, tv[i], SEC_PET);
  }
  footer("B next page   A home");
  fb.pushSprite(0, 0);
}

// ===========================================================================
// WIFI RADAR (blue = air)
// ===========================================================================
static void screenEnvironment() {
  fb.fillSprite(BG);
  header("RADAR", SEC_WIFI, wifiScanInProgress ? "scanning" : "WiFi + people", SEC_WIFI);
  panel(SEC_WIFI);

  char v[12];
  const char* k[5] = { "Nets", "Strong", "Hidden", "Open", "WPA" };
  const int   n[5] = { wifiStats.netCount, wifiStats.strongCount, wifiStats.hiddenCount,
                       wifiStats.openCount, wifiStats.wpaCount };
  for (int i = 0; i < 5; i++) {
    snprintf(v, sizeof(v), "%d", n[i]);
    kv(9, PANEL_Y + 6 + i * 14, k[i], v, SEC_WIFI, (i == 3 && n[i] > 0) ? AMBER : TEXT);
  }
  snprintf(v, sizeof(v), "%d dBm", wifiStats.avgRSSI);
  kv(9, PANEL_Y + 76, "Signal", v, SEC_WIFI, SEC_WIFI);

  const int mx = 140, base = PANEL_Y + 62;
  int level = constrain((wifiStats.avgRSSI + 100) / 12, 0, 5);
  fb.drawFastVLine(mx - 8, PANEL_Y + 4, PANEL_H - 8, SEG_OFF);
  for (int i = 0; i < 5; i++) {
    int h = 8 + i * 9;
    fb.fillRoundRect(mx + i * 17, base - h, 12, h, 2, i < level ? SEC_WIFI : SEG_OFF);
  }
  // People nearby (Bluetooth devices close by), pink = heart
  if (!radarEnabled)          textAt(mx, base + 8, "people: off", GREY);
  else if (peopleNearby < 0)  textAt(mx, base + 8, "people: soon", GREY);
  else {
    snprintf(v, sizeof(v), "%d nearby", peopleNearby);
    textAt(mx, base + 8, "people", C_PEOPLE);
    textAt(mx + 40, base + 8, v, TEXT);
    if (peopleTopKind && peopleTopKind[0]) { textAt(mx, base + 20, "mostly", GREY); textAt(mx + 40, base + 20, peopleTopKind, TEXT); }
  }
  footer("B home   A home");   // last quick page
  fb.pushSprite(0, 0);
}

// ===========================================================================
// SYSTEM (slate = the machine) + who made it
// ===========================================================================
static void screenSysInfo() {
  fb.fillSprite(BG);
  header("SYSTEM", SEC_SYSTEM, "v3.7", SEC_SYSTEM);
  panel(SEC_SYSTEM);

  char v[32];
  unsigned long s = millis() / 1000;
  int y = PANEL_Y + 4, st = 12;
  if (lastResetCount) {
    snprintf(v, sizeof(v), "Sila v3.7  %s x%lu", lastResetWhy, (unsigned long)lastResetCount);
    kv(9, y, "Firm", v, SEC_SYSTEM, AMBER);
  } else {
    kv(9, y, "Firm", "Sila v3.7", SEC_SYSTEM);
  }
  snprintf(v, sizeof(v), "%luh %02lum %02lus", s / 3600, (s / 60) % 60, s % 60);
  kv(9, y + st, "Up", v, SEC_SYSTEM);
  updateBattery();
  const char* ps = powerState == PWR_CHARGING ? "charging" : powerState == PWR_FULL ? "full" : "battery";
  snprintf(v, sizeof(v), "%d%%  %.2fV  %s", batteryPct, batteryMv / 1000.0f, ps);
  kv(9, y + 2 * st, "Batt", v, SEC_SYSTEM, powerState == PWR_CHARGING ? TEAL : powerState == PWR_FULL ? GOOD : TEXT);
  snprintf(v, sizeof(v), "%.0f C%s", chipTempC, runningHot ? "  hot - cooling" : "");
  kv(9, y + 3 * st, "Temp", v, SEC_SYSTEM, runningHot ? ALERT : chipTempC > 62 ? AMBER : TEXT);
  kv(9, y + 4 * st, "Amp", speakerAwake ? "on (playing)" : "off", SEC_SYSTEM, speakerAwake ? AMBER : TEXT);
  if (earsEnabled) snprintf(v, sizeof(v), "%.0f dB  room %.0f  beat %.0f%%", micLevelDb, micFloorDb, beatScore * 100);
  else             snprintf(v, sizeof(v), "off");
  kv(9, y + 5 * st, "Ears", v, SEC_SYSTEM, listening || dancing ? TEAL : TEXT);
  if (!radarEnabled)        snprintf(v, sizeof(v), "off");
  else if (lastRadarTime)   snprintf(v, sizeof(v), "%d near, %lus ago", peopleNearby, (millis() - lastRadarTime) / 1000);
  else                      snprintf(v, sizeof(v), "first look soon");
  kv(9, y + 6 * st, "Radar", v, SEC_SYSTEM);

  // Signature
  int sy = y + 7 * st + 1;
  drawWordmark(9, sy);
  textAt(39, sy, "by", GREY);
  textAt(57, sy, "Mohanad", TEAL);
  footer("A / B home");
  fb.pushSprite(0, 0);
}

// ===========================================================================
// CONTROLS (teal = you) / SETTINGS (yellow = tuning)
// ===========================================================================
static void screenControls(int idx) {
  fb.fillSprite(BG);
  header("CONTROLS", SEC_CONTROLS);
  static const char* const lvl[3] = { "Low", "Mid", "High" };
  static const char* const labels[CONTROLS_COUNT] = {
    "Auto Rotate", "Brightness", "Volume", "Sound", "Flip Menus", "Back"
  };
  const char* values[CONTROLS_COUNT] = {
    autoRotate ? "On" : "Off", lvl[tftBrightnessIndex % 3], lvl[volumeIndex % 3],
    soundMode == SOUND_ALL ? "All" : soundMode == SOUND_KEY ? "Key" : "Off",
    screenFlipped ? "Yes" : "No", nullptr
  };
  menuList(labels, values, CONTROLS_COUNT, idx, SEC_CONTROLS);
  footer("B next   A change   hold B back");
  fb.pushSprite(0, 0);
}

static void screenSettings(int idx) {
  fb.fillSprite(BG);
  header("SETTINGS", SEC_SETTINGS);
  char save[8];
  snprintf(save, sizeof(save), "%us", autoSaveMs / 1000);
  static const char* const labels[SETTINGS_COUNT] = {
    "Idle Screen", "Lie-down Sleep", "Ears (mic)", "People Radar", "Scan Rate", "Auto Save",
    "Reset Stats", "New Egg", "Back"
  };
  const char* values[SETTINGS_COUNT] = {
    idleMode == IDLE_DIM ? "Dim 1min" : idleMode == IDLE_BLACK ? "Off 1min" : "Always on",
    lieDownSleep ? "On" : "Off", earsEnabled ? "On" : "Off", radarEnabled ? "On" : "Off",
    scanSaver ? "Saver" : "Normal", save, nullptr, nullptr, nullptr
  };
  menuList(labels, values, SETTINGS_COUNT, idx, SEC_SETTINGS);
  footer("B next   A select   hold B back");
  fb.pushSprite(0, 0);
}

// ===========================================================================
// GAME OVER
// ===========================================================================
static void screenGameOver() {
  stepFacePhysics();
  faceRenderDead();
  composeFace();
  textCenter(6, "your pet has faded away", TERRA);
  if ((millis() / PRESS_HINT_PULSE_MS) % 2) textCenter(116, "[ A ] new egg", TEAL);
  fb.pushSprite(0, 0);
}

// ===========================================================================
// PUBLIC API
// ===========================================================================
void uiInit() {
  fb.setTextFont(1);
  fb.setTextSize(1);
  fb.setTextWrap(false, false);               // text running off-screen must not wrap to the next line
  unsigned long now = millis();
  hatchFrameUi = 0;
  lastHatchFrameUi = hatchScreenSince = now;
  // Landscape (used most) in internal RAM for speed, portrait in PSRAM so the
  // radios keep enough internal RAM. Either falls back to PSRAM.
  M5Canvas* cvs[2] = { &faceLand, &facePort };
  for (int i = 0; i < 2; i++) {
    M5Canvas& c = *cvs[i];
    if (c.width() > 0) continue;              // already made
    int w = i == 0 ? SCR_W : SCR_H, h = i == 0 ? SCR_H : SCR_W;
    c.setColorDepth(16);
    c.setTextFont(1);
    c.setTextWrap(false, false);
    c.setPsram(i == 1);
    if (!c.createSprite(w, h)) { c.setPsram(true); c.createSprite(w, h); }
  }
  faceCanvas = &faceLand;
}

void uiOnScreenChange(Screen s) {
  int idx = -1;
  if (s == SCREEN_CONTROLS) idx = controlsIndex;
  if (s == SCREEN_SETTINGS) idx = settingsMenuIndex;
  if (idx >= 0) {
    int cnt = s == SCREEN_CONTROLS ? CONTROLS_COUNT : SETTINGS_COUNT;
    menuY = menuTargetY = rowY(idx - listFirst(cnt, idx));
  }
  if (s == SCREEN_MENU) { carouselPos = mainMenuIndex; lastCarousel = 0; }

  if (s == SCREEN_HATCH) { hatchFrameUi = 0; lastHatchFrameUi = hatchScreenSince = millis(); }
  // Coming home from the boot intro or the egg: keep the face as it is
  if (s == SCREEN_HOME && lastScreen != SCREEN_BOOT && lastScreen != SCREEN_HATCH) faceReset();
  if (s == SCREEN_BOOT) faceStartIntro(true);
  lastScreen = s;
}

void uiDrawScreen(Screen screen, int mainIdx, int ctlIdx, int setIdx) {
  if (screen == SCREEN_CONTROLS) menuTargetY = rowY(ctlIdx - listFirst(CONTROLS_COUNT, ctlIdx));
  if (screen == SCREEN_SETTINGS) {
    static int lastFirst = 0;
    int f = listFirst(SETTINGS_COUNT, setIdx);
    if (f != lastFirst) { menuY = rowY(setIdx - f); lastFirst = f; }   // scrolled: jump, no slide
    menuTargetY = rowY(setIdx - f);
  }

  switch (screen) {
    case SCREEN_BOOT:        screenBoot();           break;
    case SCREEN_HATCH:       screenHatch();          break;
    case SCREEN_HOME:        screenHome();           break;
    case SCREEN_MENU:        screenMenu(mainIdx);    break;
    case SCREEN_VITALS:      screenVitals();         break;
    case SCREEN_WALKS:       screenWalks();          break;
    case SCREEN_PET_STATUS:  screenPetStatus();      break;
    case SCREEN_ENVIRONMENT: screenEnvironment();    break;
    case SCREEN_SYSINFO:
    case SCREEN_DIAGNOSTICS: screenSysInfo();        break;
    case SCREEN_CONTROLS:    screenControls(ctlIdx); break;
    case SCREEN_SETTINGS:    screenSettings(setIdx); break;
    case SCREEN_GAMEOVER:    screenGameOver();       break;
    case SCREEN_GAME:        gameDraw();             break;
  }
}
