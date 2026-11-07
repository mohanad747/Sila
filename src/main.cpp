// Sila - a WiFi-fed companion for the M5StickS3, made by Mohanad.
// "Sila" (صلة) means connection / bond. Started as a port of cifertech/TamaFi (MIT).
//
// Hardware mapping vs the original TamaFi V2 board:
//   ST7789 240x240 + TFT_eSPI  ->  M5.Display 240x135 (landscape) + M5Canvas
//   6 tactile buttons          ->  BtnA (front) + BtnB (side) + shake (IMU)
//   PWM buzzer                 ->  M5.Speaker (ES8311 codec + amp)
//   4x WS2812 NeoPixels        ->  expressions and icons on the face
//   TP4056 charger             ->  M5PM1 via M5.Power (battery % in header)
//
// Controls:
//   BtnA (front, big)  : OK / select / open menu
//   BtnB click (side)  : next item / cycle quick pages on home
//   BtnB hold          : back
//   BtnA hold (home)   : pat the pet
//   Shake              : play with the pet (+happiness)
//   Lay it down flat   : the pet goes to bed; pick it up to wake it
//   Any input          : brightens the screen when it has dimmed

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <math.h>
#include "senses.h"
#include "backup.h"
#include <esp_task_wdt.h>
#include <esp_system.h>

#include "ui.h"
#include "ui_anim.h"
#include "shows.h"

// ---------------- Tunables ----------------
static const uint32_t SCREEN_SLEEP_MS   = 60000;  // dim (or turn off) after this much idle time
static const uint8_t  DIM_BRIGHTNESS    = 14;     // backlight level while dimmed
static const uint32_t LIE_DOWN_MS       = 4000;   // flat and still this long = bedtime
static const float    STILL_TOLERANCE_G = 0.06f;  // movement smaller than this counts as "still"
static const uint32_t NO_WIFI_SICK_MS   = 3UL * 3600UL * 1000UL;  // no WiFi this long -> sick (just bored before)
static const uint32_t BED_HEAL_MS       = 60000;  // in bed: +1 health per minute
static const uint32_t SPEAKER_OFF_MS    = 300;    // power the amp down this long after the last sound
static const uint32_t ROTATE_STABLE_MS  = 300;    // new orientation must hold this long
static const float    FLAT_XY_G         = 0.17f;  // lying flat = within ~10 deg of level
static const float    WAKE_XY_G         = 0.34f;  // in bed: tilted past ~20 deg = picked up
static const float    WAKE_JOLT_G       = 0.25f;  // in bed: a bump this big = picked up
static const int      DIZZY_TURNS       = 4;      // this many orientation changes...
static const uint32_t DIZZY_WINDOW_MS   = 10000;  // ...within this time = dizzy
static const uint32_t DIZZY_MS          = 6000;   // spiral eyes this long
static const uint32_t QUEASY_MS         = 45000;  // then feels sick this long
static const uint32_t DIZZY_COST_MS     = 300000; // spinning costs stats at most this often (a swinging keychain turns a lot)
static const int      SICK_BELOW        = 25;     // health under this = sick...
static const int      WELL_FROM         = 32;     // ...until it is back up to this (no flickering at the line)
static const uint32_t PAT_COOLDOWN_MS   = 8000;
static const float    SHOCK_JOLT_G      = 0.9f;   // a bump this hard while asleep = wakes in shock
static const float    SHOCK_SOUND_DB    = 38;     // only a bang this loud wakes it in shock (else it just stirs)
static const uint32_t ROCKED_NAP_MS     = 15UL * 60UL * 1000UL;  // rocked to sleep: wakes by itself after this
static const uint32_t STEP_REWARD_EVERY = 100;    // +1 happiness and +1 health per this many steps
static const uint32_t STEP_GOAL         = 3000;
static const float    SHAKE_THRESHOLD_G  = 0.7f;  // jolt size (g away from rest) that counts as one peak
static const int      SHAKE_PEAKS_NEEDED = 3;     // peaks needed...
static const uint32_t SHAKE_WINDOW_MS    = 800;   // ...within this window
static const uint32_t SHAKE_PEAK_GAP_MS  = 70;    // min gap between peaks
static const uint32_t SHAKE_DEBOUNCE_MS  = 1000;
static const uint32_t PLAY_EFFECT_MS    = 1500;

// ---------------- Global objects ----------------
M5Canvas    fb(&M5.Display);
Preferences prefs;

// ---------------- Shared game state ----------------
Screen    currentScreen   = SCREEN_BOOT;
Activity  currentActivity = ACT_NONE;
RestPhase restPhase       = REST_NONE;

Pet       pet;
WifiStats wifiStats;

Mood      currentMood = MOOD_CALM;
Stage     petStage    = STAGE_BABY;

bool      hasHatchedOnce = false;
bool      hatchTriggered = false;

bool      hungerEffectActive = false;
int       hungerEffectFrame  = 0;

bool          wifiScanInProgress = false;
unsigned long lastWifiScanTime   = 0;
unsigned long lastSaveTime       = 0;

uint8_t   soundMode          = SOUND_KEY;
uint8_t   tftBrightnessIndex = 1;
uint8_t   volumeIndex        = 1;
bool      screenFlipped      = false;
uint8_t   idleMode           = IDLE_ON;
bool      lieDownSleep       = true;
bool      petBedtime         = false;   // asleep because it was laid down
bool      scanSaver          = false;   // Settings > Scan Rate: Saver = fewer WiFi scans
bool      autoRotate         = true;    // face turns with the stick
float     faceRoll           = 0;       // -1..1 sideways lean, face tilts with it
uint8_t   faceTargetRot      = 1;       // display rotation the face should look upright in
float     gravAngle          = 0;       // continuous angle that keeps the face level
bool      gravValid          = false;
float     motionX = 0, motionY = 0;     // movement in screen space, gravity removed
uint8_t   baseRot            = 1;       // rotation the panel is actually set to (1, or 3 = Flip Menus)
unsigned long dizzyUntil     = 0;       // spiral eyes
unsigned long queasyUntil    = 0;       // feels sick after being spun around
bool      speakerAwake       = false;   // for the System page
bool      rockedSleep        = false;   // asleep because you rocked it
unsigned long startleUntil   = 0;       // shocked face
unsigned long stirUntil      = 0;       // stirs in its sleep
unsigned long noticeUntil    = 0;       // heard a clap / knock: looks at you
unsigned long stepGoalUntil  = 0;       // celebrating the daily step goal
unsigned long calledUntil    = 0;       // double clap: "you called?"
unsigned long giggleUntil    = 0;       // poked: giggles
unsigned long grumpyUntil    = 0;       // poked too much
uint8_t       pokeLevel      = 0;
unsigned long fallUntil      = 0;       // falling: screams
unsigned long wheeUntil      = 0;       // tossed and caught softly
unsigned long ouchUntil      = 0;       // landed hard
unsigned long zapUntil       = 0;       // just plugged in: eats electricity
uint16_t      gameBest       = 0;       // best Catch score

// Power (read here, shown by the UI)
uint8_t   powerState         = PWR_BATTERY;
int       batteryShown       = -1;      // smoothed %, never jumps around
float     chipTempC          = 0;       // ESP32-S3 die temperature
bool      runningHot         = false;
uint16_t  autoSaveMs         = 30000;

uint8_t   traitCuriosity = 70;
uint8_t   traitActivity  = 60;
uint8_t   traitStress    = 40;

unsigned long lastDecisionTime        = 0;
uint32_t      currentDecisionInterval = 10000;

int       restFrameIndex = 0;

int       mainMenuIndex     = 0;
int       controlsIndex     = 0;
int       settingsMenuIndex = 0;

unsigned long playEffectUntil = 0;

// ---------------- Internal timers ----------------
static unsigned long hungerTimer    = 0;
static unsigned long happinessTimer = 0;
static unsigned long healthTimer    = 0;
static unsigned long ageTimer       = 0;
static unsigned long lastLogicTick  = 0;

static unsigned long lastRestAnimTime = 0;
static unsigned long restPhaseStart   = 0;
static unsigned long restDurationMs   = 0;
static bool          restStatsApplied = false;

static unsigned long lastHungerFrameTime = 0;

static unsigned long lastInputTime = 0;
enum ScreenState { SCR_ON, SCR_DIM, SCR_OFF };
static ScreenState   screenState   = SCR_ON;
static bool          screenAsleep  = false;   // true while dimmed or off

// IMU-derived state (updated in readShake)
static bool          lyingFlat     = false;
static float         imuX = 0, imuY = 0, imuZ = 1;   // last accel reading (g)
float imuDebugX = 0, imuDebugY = 0;                  // shown on the System page
static unsigned long noWifiSince   = 0;             // first scan that found nothing (0 = WiFi seen)
static unsigned long bedHealTimer  = 0;
static unsigned long speakerIdleSince = 0;
static uint8_t       displayRot    = 255;   // 255 = not set yet, forces the first setRotation()
static unsigned long flatSince     = 0;
static unsigned long lastMoveTime  = 0;
static unsigned long lastShakeTime = 0;
static unsigned long lastPatTime   = 0;
static unsigned long lastJoltTime  = 0;
static unsigned long strongJoltTime = 0;
static unsigned long rockedSince   = 0;
static unsigned long noSleepUntil  = 0;
char     lastResetWhy[16] = "";
uint32_t lastResetCount   = 0;   // just woken by a shock: no dozing off right away
static unsigned long lastImuRead   = 0;

// Upstream used 8-15 s. On a 250 mAh keychain that means a WiFi scan every few
// seconds, so the pet now thinks a bit slower.
static const uint32_t DECISION_INTERVAL_MIN = 20000;
static const uint32_t DECISION_INTERVAL_MAX = 45000;
// Stat decay, tuned for a pet you carry all day (upstream drained a full
// pet in a few minutes). Each timer removes 1 point (2 when neglected/lonely).
//   hunger    -1 / 3 min -> 20 per hour (full to empty in ~5 h without WiFi)
//   happiness -1 / 3 min -> 20 per hour
//   health    -1 / 5 min -> 12 per hour
static const uint32_t HUNGER_DECAY_MS = 180000;
static const uint32_t HAPPY_DECAY_MS  = 180000;
static const uint32_t HEALTH_DECAY_MS = 300000;
static const int      MIN_MEAL        = 10;     // any successful hunt feeds at least this much

// =====================================================================
//  Glow (replaces NeoPixels): just sets a colour the UI draws as a frame
// =====================================================================
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return M5.Display.color565(r, g, b); }

void ledsOff() {}
void ledsHappy() {}
void ledsSad() {}
void ledsWifi() {}
void ledsRest() {}

// =====================================================================
//  Sound: same retro sequences as TamaFi, played through M5.Speaker
// =====================================================================
struct RetroSound {
  const int *freqs;
  const int *times;
  int length;
};

static const int CLICK_FREQS[] = { 2100, 1600, 900 };
static const int CLICK_TIMES[] = {   20,   20,  20 };
static const int GOOD_FREQS[]  = { 600, 900, 1200, 1500 };
static const int GOOD_TIMES[]  = {  40,  40,   40,   60 };
static const int BAD_FREQS[]   = { 900, 700, 500, 300 };
static const int BAD_TIMES[]   = {  50,  50,  60,  80 };
static const int DISC_FREQS[]  = { 400, 650, 900, 1200, 1500 };
static const int DISC_TIMES[]  = {  40,  40,  40,   40,   60 };
static const int RSTS_FREQS[]  = { 600, 400, 300 };
static const int RSTS_TIMES[]  = {  60,  70,  90 };
static const int RSTE_FREQS[]  = { 300, 500, 700 };
static const int RSTE_TIMES[]  = {  60,  60,  80 };
static const int HATCH_FREQS[] = { 500, 800, 1200, 1600, 2000 };
static const int HATCH_TIMES[] = {  60,  60,   60,   80,  100 };
static const int PLAY_FREQS[]  = { 1200, 1500, 1200, 1800 };
static const int SHOCK_FREQS[] = { 2600, 1900, 2800 };
static const int SHOCK_TIMES[] = {   40,   30,   60 };
static const int PLAY_TIMES[]  = {   50,   50,   50,   90 };
static const int CALL_FREQS[]  = { 700, 1100, 1500 };               // "hm? yes?"
static const int CALL_TIMES[]  = {  60,   60,  110 };
static const int GIGL_FREQS[]  = { 1500, 1250, 1700, 1400, 1950 };  // hehe
static const int GIGL_TIMES[]  = {   35,   35,   35,   35,   55 };
static const int GRMP_FREQS[]  = { 420, 330 };                      // hmph
static const int GRMP_TIMES[]  = {  90, 140 };
static const int FALL_FREQS[]  = { 2600, 2500, 2400, 2300, 2200, 2100, 2000, 1900, 1800, 1700 };   // AAAAH
static const int FALL_TIMES[]  = {   45,   45,   45,   45,   45,   45,   45,   45,   45,   60 };
static const int WHEE_FREQS[]  = { 900, 1300, 1800, 2400 };
static const int WHEE_TIMES[]  = {  50,   50,   50,   90 };
static const int OUCH_FREQS[]  = { 1400, 500 };
static const int OUCH_TIMES[]  = {   40, 160 };
static const int ZAP_FREQS[]   = { 220, 1900, 260, 2100, 1600, 2400 };   // bzzt... yum
static const int ZAP_TIMES[]   = {  30,   20,  30,   20,   60,   90 };
static const int CTCH_FREQS[]  = { 1800, 2500 };                    // game: caught
static const int CTCH_TIMES[]  = {   25,   40 };
static const int HURT_FREQS[]  = { 330, 220 };                      // game: hurt
static const int HURT_TIMES[]  = {  60, 100 };
static const int OVER_FREQS[]  = { 800, 600, 450, 300 };            // game over
static const int OVER_TIMES[]  = { 110, 110, 110, 220 };

static const RetroSound SOUNDS[] = {
  { CLICK_FREQS, CLICK_TIMES, 3 },  // 0 click
  { GOOD_FREQS,  GOOD_TIMES,  4 },  // 1 good feed
  { BAD_FREQS,   BAD_TIMES,   4 },  // 2 bad feed
  { DISC_FREQS,  DISC_TIMES,  5 },  // 3 discover / evolve
  { RSTS_FREQS,  RSTS_TIMES,  3 },  // 4 rest start
  { RSTE_FREQS,  RSTE_TIMES,  3 },  // 5 rest end
  { HATCH_FREQS, HATCH_TIMES, 5 },  // 6 hatch
  { PLAY_FREQS,  PLAY_TIMES,  4 },  // 7 play (new)
  { SHOCK_FREQS, SHOCK_TIMES, 3 },  // 8 startled
  { CALL_FREQS,  CALL_TIMES,  3 },  // 9 called (double clap)
  { GIGL_FREQS,  GIGL_TIMES,  5 },  // 10 giggle (poked)
  { GRMP_FREQS,  GRMP_TIMES,  2 },  // 11 grumpy (poked too much)
  { FALL_FREQS,  FALL_TIMES, 10 },  // 12 falling scream
  { WHEE_FREQS,  WHEE_TIMES,  4 },  // 13 whee (caught softly)
  { OUCH_FREQS,  OUCH_TIMES,  2 },  // 14 ouch (hard landing)
  { ZAP_FREQS,   ZAP_TIMES,   6 },  // 15 eats electricity
  { CTCH_FREQS,  CTCH_TIMES,  2 },  // 16 game: catch
  { HATCH_FREQS, HATCH_TIMES, 5 },  // 17 game: heart
  { HURT_FREQS,  HURT_TIMES,  2 },  // 18 game: hurt
  { OVER_FREQS,  OVER_TIMES,  4 },  // 19 game over
  { CLICK_FREQS, CLICK_TIMES, 3 },  // 20 game: start
};
static const int SOUND_COUNT = sizeof(SOUNDS) / sizeof(SOUNDS[0]);

static int           sndIndex = -1;
static void applyVolumeRaw();
static int           sndStep  = 0;
static unsigned long sndNext  = 0;

// Sound modes: SOUND_OFF, SOUND_KEY (only things you did + big moments),
// SOUND_ALL (also every autonomous hunt / explore / nap, like the original).
// "Ambient" sounds never play while the screen is asleep.
// The StickS3 speaker amp hisses/buzzes from charger noise when it is left on,
// so it is only powered while a sound is actually playing.
static void speakerOn() {
  micPause();                         // the mic and speaker share the audio pins
  if (!M5.Speaker.isEnabled() || !M5.Speaker.isRunning()) {
    M5.Speaker.begin();
    applyVolumeRaw();
  }
  speakerAwake = true;
  speakerIdleSince = 0;
}

static void speakerIdleCheck() {
  if (!M5.Speaker.isRunning() || sndIndex >= 0 || M5.Speaker.isPlaying()) return;
  unsigned long now = millis();
  if (speakerIdleSince == 0) { speakerIdleSince = now; return; }
  if (now - speakerIdleSince > SPEAKER_OFF_MS) {
    M5.Speaker.end();                       // turns the AW8737 amp off
    // M5Unified leaves the ES8311 codec running; power its DAC and analog
    // stage down too (begin() re-initialises all of these registers).
    const uint8_t ES8311 = 0x18;
    M5.In_I2C.writeRegister8(ES8311, 0x32, 0x00, 100000);   // DAC volume -> mute
    M5.In_I2C.writeRegister8(ES8311, 0x13, 0x00, 100000);   // output driver off
    M5.In_I2C.writeRegister8(ES8311, 0x12, 0x02, 100000);   // DAC power down
    M5.In_I2C.writeRegister8(ES8311, 0x0D, 0xFA, 100000);   // analog power down
    speakerIdleSince = 0;
    speakerAwake = false;
    micResume();                      // ears back on
  }
}

static void sndStart(int idx, bool ambient = false) {
  if (soundMode == SOUND_OFF) return;
  if (ambient && (soundMode != SOUND_ALL || screenAsleep)) return;
  speakerOn();
  sndIndex = idx;
  sndStep  = 0;
  sndNext  = 0;
}

void sndUpdate() {
  if (soundMode == SOUND_OFF || sndIndex < 0) { sndIndex = -1; return; }

  unsigned long now = millis();
  if (now < sndNext) return;

  if (sndIndex >= SOUND_COUNT) { sndIndex = -1; return; }
  const RetroSound &snd = SOUNDS[sndIndex];

  if (sndStep >= snd.length) {
    M5.Speaker.stop();
    sndIndex = -1;
    sndStep  = 0;
    return;
  }

  M5.Speaker.tone(snd.freqs[sndStep], snd.times[sndStep]);
  sndNext = now + snd.times[sndStep];
  sndStep++;
}

void sndClick()     { sndStart(0); }
void sndGoodFeed()  { sndStart(1, true); ledsHappy(); }
void sndBadFeed()   { sndStart(2, true); ledsSad(); }
void sndDiscover()  { sndStart(3, true); ledsWifi(); }
void sndRestStart() { sndStart(4, true); }
void sndRestEnd()   { sndStart(5, true); }
void sndHatch()     { sndStart(6); }
void sndPlay()      { sndStart(7); }
void sndShock()     { sndStart(8); }
void sndEvolve()    { sndStart(6); ledsHappy(); }   // always worth hearing
void sndGame(int kind) { sndStart(16 + constrain(kind, 0, 4)); }

// =====================================================================
//  Display / speaker settings
// =====================================================================
static const uint8_t BRIGHTNESS_LEVELS[3] = { 40, 110, 200 };
static const uint8_t VOLUME_LEVELS[3]     = { 60, 130, 220 };

// The backlight glides to its new level (~0.3 s) instead of jumping: waking
// from a dim sleep no longer slams the battery with backlight + speaker +
// radio all at the same instant.
static int brightNow = -1, brightTarget = 0;
void applyTftBrightness() {
  brightTarget = screenState == SCR_ON  ? BRIGHTNESS_LEVELS[tftBrightnessIndex % 3]
               : screenState == SCR_DIM ? DIM_BRIGHTNESS : 0;
  if (brightNow < 0 || brightTarget < brightNow) {        // dimming / first set: immediate
    brightNow = brightTarget;
    M5.Display.setBrightness(brightNow);
  }
}
static void brightnessTick() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (brightNow == brightTarget || now - last < 15) return;
  last = now;
  brightNow = min(brightTarget, brightNow + 12);
  M5.Display.setBrightness(brightNow);
}

static void applyVolumeRaw() { M5.Speaker.setVolume(VOLUME_LEVELS[volumeIndex % 3]); }
void applyVolume() { applyVolumeRaw(); }

// ---------------------------------------------------------------------
// Orientation. Menus and pages are always landscape (1, or 3 when Flip
// Screen is on). The face on the home screen follows gravity instead, in
// all four directions, so it is upright whichever way the stick hangs.
//
// Accelerometer "up" vector -> display rotation. Assumes the IMU axes follow
// the rotation-0 (portrait) screen: +X to the right, +Y towards the top.
// If the face comes out upside down in some pose, swap the pair here.
// ---------------------------------------------------------------------
static const uint8_t ROT_UP_POS_X = 1, ROT_UP_NEG_X = 3;
static const uint8_t ROT_UP_POS_Y = 0, ROT_UP_NEG_Y = 2;

static int8_t        rotCandidate      = -1;
static unsigned long rotCandidateSince = 0;
static uint8_t       faceRot           = 1;

// Spin it around a lot and it gets dizzy (then queasy for a while)
static void noteTurn(unsigned long now) {
  static unsigned long turns[DIZZY_TURNS] = {0};
  static int idx = 0;
  turns[idx] = now;
  idx = (idx + 1) % DIZZY_TURNS;
  unsigned long oldest = turns[idx];
  if (oldest == 0 || now - oldest > DIZZY_WINDOW_MS) return;
  if (currentScreen != SCREEN_HOME || restPhase != REST_NONE) return;

  if (now >= dizzyUntil) {                       // new dizzy spell: costs a little
    static unsigned long lastCost = 0;
    if (lastCost == 0 || now - lastCost > DIZZY_COST_MS) {
      lastCost = now;
      pet.happiness = constrain(pet.happiness - 4, 0, 100);
      pet.health    = constrain(pet.health - 3,    0, 100);
    }
    sndStart(2);                               // always audible, not an "ambient" sound
  }
  dizzyUntil  = now + DIZZY_MS;
  queasyUntil = now + DIZZY_MS + QUEASY_MS;
  for (auto &t : turns) t = 0;
}

static uint8_t gravityRotation() {
  if (fabsf(imuZ) > 0.8f) return faceRot;                 // lying flat: keep what we have
  float ax = fabsf(imuX), ay = fabsf(imuY);
  int8_t cand = -1;
  if (ax > 0.55f && ax - ay > 0.25f)      cand = imuX > 0 ? ROT_UP_POS_X : ROT_UP_NEG_X;
  else if (ay > 0.55f && ay - ax > 0.25f) cand = imuY > 0 ? ROT_UP_POS_Y : ROT_UP_NEG_Y;
  if (cand < 0) return faceRot;                           // in between: hysteresis

  unsigned long now = millis();
  if (cand != rotCandidate) { rotCandidate = cand; rotCandidateSince = now; }
  else if (now - rotCandidateSince >= ROTATE_STABLE_MS && faceRot != cand) {
    faceRot = cand;
    noteTurn(now);
  }
  return faceRot;
}

// Sideways lean (-1..1) of "up" relative to the current screen's right edge
static float sidewaysLean(uint8_t rot) {
  switch (rot) {
    case 0:  return  imuX;
    case 2:  return -imuX;
    case 1:  return -imuY;
    default: return  imuY;
  }
}

// The panel itself always stays landscape. The face is drawn into its own
// canvas and the UI rotates that canvas smoothly toward faceTargetRot.
void applyRotation() {
  baseRot = screenFlipped ? 3 : 1;
  if (displayRot != baseRot) { displayRot = baseRot; M5.Display.setRotation(baseRot); }

  bool faceMode = (currentScreen == SCREEN_HOME || currentScreen == SCREEN_BOOT) &&
                  autoRotate && M5.Imu.isEnabled();
  if (!faceMode) { faceRot = baseRot; faceTargetRot = baseRot; faceRoll = 0; gravValid = false; return; }
  faceTargetRot = gravityRotation();       // still used to count turns (dizzy)

  // Continuous angle: "up" in screen space -> rotation that keeps the face level.
  // Matches the verified quarter mapping: up = screen-up -> 0, screen-right -> 90.
  float upR = baseRot == 1 ? -imuY : imuY;
  float upU = baseRot == 1 ?  imuX : -imuX;
  gravValid = fabsf(imuZ) < 0.85f && (upR * upR + upU * upU) > 0.08f;
  if (gravValid) gravAngle = atan2f(upR, upU) * RAD_TO_DEG;
  faceRoll = fabsf(imuZ) < 0.8f ? constrain(sidewaysLean(faceTargetRot), -1.0f, 1.0f) : 0;
}

// =====================================================================
//  WiFi scan (passive network counting - same as TamaFi)
// =====================================================================
void startWifiScan() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.scanNetworks(true);
  wifiScanInProgress = true;
}

static bool scanFailed = false;

bool checkWifiScanDone() {
  if (!wifiScanInProgress) return false;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return false;

  wifiScanInProgress = false;
  lastWifiScanTime   = millis();

  if (n < 0) {                 // the scan itself failed: says nothing about the WiFi around, so no food and no sulking
    scanFailed = true;
    Serial.printf("WIFI-EV scan failed (%d)\n", n);
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    return true;
  }
  scanFailed = false;

  WifiStats s;
  s.netCount = n;
  int totalRSSI = 0;

  for (int i = 0; i < n; i++) {
    int rssi = WiFi.RSSI(i);
    totalRSSI += rssi;
    if (rssi > -60) s.strongCount++;
    if (WiFi.SSID(i).length() == 0) s.hiddenCount++;
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) s.openCount++;
    else s.wpaCount++;
  }

  s.avgRSSI = (n > 0) ? (totalRSSI / n) : -100;
  wifiStats = s;
  if (n > 0) noWifiSince = 0;
  else if (noWifiSince == 0) noWifiSince = millis();

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);    // radio off until the next scan
  return true;
}

// =====================================================================
//  Persistence
// =====================================================================
// ---- Backup outside the firmware area (survives re-flashing, see backup.cpp) ----
static unsigned long lastBackup = 0;
static bool backupForce = true;            // first save after boot always backs up

static void writeBackup() {
  SaveBlob b = {};
  b.hunger = pet.hunger; b.happiness = pet.happiness; b.health = pet.health;
  b.ageMin = pet.ageMinutes; b.ageHr = pet.ageHours; b.ageDay = pet.ageDays;
  b.stage = (uint8_t)petStage; b.hatched = hasHatchedOnce;
  b.tCur = traitCuriosity; b.tAct = traitActivity; b.tStr = traitStress;
  b.stToday = stepsToday; b.stTotal = stepsTotal; b.stBest = stepsBest; b.stDay = stepsDayIdx;
  b.soundMode = soundMode; b.bright = tftBrightnessIndex; b.volume = volumeIndex;
  b.flags = (autoRotate ? 1 : 0) | (lieDownSleep ? 2 : 0) | (earsEnabled ? 4 : 0) |
            (radarEnabled ? 8 : 0) | (scanSaver ? 16 : 0) | (screenFlipped ? 32 : 0);
  b.idleMode = idleMode;
  b.gameBest = gameBest;
  b.idleShows1 = idleShows + 1;
  backupWrite(b);
  lastBackup = millis();
  backupForce = false;
}

static bool restoreFromBackup() {
  SaveBlob b;
  if (!backupLoad(b) || !b.hatched) return false;
  pet.hunger = b.hunger; pet.happiness = b.happiness; pet.health = b.health;
  pet.ageMinutes = b.ageMin; pet.ageHours = b.ageHr; pet.ageDays = b.ageDay;
  petStage = (Stage)constrain((int)b.stage, 0, 3); hasHatchedOnce = true;
  traitCuriosity = b.tCur; traitActivity = b.tAct; traitStress = b.tStr;
  stepsToday = b.stToday; stepsTotal = b.stTotal; stepsBest = b.stBest; stepsDayIdx = b.stDay;
  soundMode = b.soundMode > SOUND_ALL ? SOUND_KEY : b.soundMode;
  tftBrightnessIndex = b.bright % 3; volumeIndex = b.volume % 3;
  autoRotate = b.flags & 1; lieDownSleep = b.flags & 2; earsEnabled = b.flags & 4;
  radarEnabled = b.flags & 8; scanSaver = b.flags & 16; screenFlipped = b.flags & 32;
  idleMode = b.idleMode > IDLE_ON ? IDLE_ON : b.idleMode;
  gameBest = b.gameBest;
  if (b.idleShows1 >= 1 && b.idleShows1 <= 3) idleShows = b.idleShows1 - 1;
  return true;
}

void saveState() {
  prefs.putInt("hunger", pet.hunger);
  prefs.putInt("happy",  pet.happiness);
  prefs.putInt("health", pet.health);

  prefs.putULong("ageMin", pet.ageMinutes);
  prefs.putULong("ageHr",  pet.ageHours);
  prefs.putULong("ageDay", pet.ageDays);

  prefs.putUChar("stage",  (uint8_t)petStage);
  prefs.putBool("hatched", hasHatchedOnce);

  prefs.putUChar("sndMode", soundMode);
  prefs.putUChar("tftBri", tftBrightnessIndex);
  prefs.putUChar("vol",    volumeIndex);
  prefs.putBool("flip",    screenFlipped);
  prefs.putUChar("idle2",  idleMode);
  prefs.putBool("lieDown", lieDownSleep);
  prefs.putBool("saver",   scanSaver);
  prefs.putBool("aRot",    autoRotate);
  prefs.putBool("ears",    earsEnabled);
  prefs.putBool("radar",   radarEnabled);
  prefs.putULong("stToday", stepsToday);
  prefs.putULong("stTotal", stepsTotal);
  prefs.putULong("stBest",  stepsBest);
  prefs.putUShort("stDay",  stepsDayIdx);
  prefs.putUShort("gBest",  gameBest);
  prefs.putUChar("shows",   idleShows);

  // Backup every 5 minutes, and right away when forced or the pet just hatched
  static bool backedUpHatched = false;
  if (hasHatchedOnce != backedUpHatched) backupForce = true;
  if (backupForce || millis() - lastBackup > 300000UL) { writeBackup(); backedUpHatched = hasHatchedOnce; }
  prefs.putUShort("aSave", autoSaveMs);

  prefs.putUChar("tCur", traitCuriosity);
  prefs.putUChar("tAct", traitActivity);
  prefs.putUChar("tStr", traitStress);
}

void loadState() {
  int h = prefs.getInt("hunger", -1);
  if (h == -1 && restoreFromBackup()) {
    // The save area was wiped by a re-flash: the pet comes back from its backup
    saveState();
    return;
  }
  if (h == -1) {
    // First boot: fresh pet with a random personality
    pet.hunger = pet.happiness = pet.health = 70;
    pet.ageMinutes = pet.ageHours = pet.ageDays = 0;
    petStage       = STAGE_BABY;
    hasHatchedOnce = false;

    traitCuriosity = random(40, 90);
    traitActivity  = random(30, 90);
    traitStress    = random(20, 80);

    saveState();
    return;
  }

  pet.hunger     = prefs.getInt("hunger", 70);
  pet.happiness  = prefs.getInt("happy",  70);
  pet.health     = prefs.getInt("health", 70);
  pet.ageMinutes = prefs.getULong("ageMin", 0);
  pet.ageHours   = prefs.getULong("ageHr",  0);
  pet.ageDays    = prefs.getULong("ageDay", 0);

  petStage       = (Stage)prefs.getUChar("stage", (uint8_t)STAGE_BABY);
  hasHatchedOnce = prefs.getBool("hatched", false);

  soundMode          = prefs.getUChar("sndMode", SOUND_KEY);
  if (soundMode > SOUND_ALL) soundMode = SOUND_KEY;
  tftBrightnessIndex = prefs.getUChar("tftBri", 1) % 3;
  volumeIndex        = prefs.getUChar("vol", 1) % 3;
  screenFlipped      = prefs.getBool("flip", false);
  idleMode           = prefs.getUChar("idle2", IDLE_ON);   // new key: default is now Always on
  if (idleMode > IDLE_ON) idleMode = IDLE_ON;
  lieDownSleep       = prefs.getBool("lieDown", true);
  scanSaver          = prefs.getBool("saver", false);
  autoRotate         = prefs.getBool("aRot", true);
  earsEnabled        = prefs.getBool("ears", true);
  radarEnabled       = prefs.getBool("radar", true);
  stepsToday         = prefs.getULong("stToday", 0);
  stepsTotal         = prefs.getULong("stTotal", 0);
  stepsBest          = prefs.getULong("stBest", 0);
  stepsDayIdx        = prefs.getUShort("stDay", (uint16_t)pet.ageDays);
  gameBest           = prefs.getUShort("gBest", 0);
  idleShows          = prefs.getUChar("shows", SHOWS_LIVELY);
  if (idleShows > SHOWS_OFF) idleShows = SHOWS_LIVELY;
  autoSaveMs         = prefs.getUShort("aSave", 30000);

  traitCuriosity = prefs.getUChar("tCur", 70);
  traitActivity  = prefs.getUChar("tAct", 60);
  traitStress    = prefs.getUChar("tStr", 40);
}

// =====================================================================
//  Mood & evolution (unchanged from TamaFi)
// =====================================================================
void updateMood() {
  bool longNoWifi = noWifiSince != 0 && millis() - noWifiSince > NO_WIFI_SICK_MS;
  bool queasy = millis() < queasyUntil;
  static bool weak = false;
  if (pet.health < SICK_BELOW) weak = true;
  else if (pet.health >= WELL_FROM) weak = false;
  if (weak || longNoWifi || queasy) { currentMood = MOOD_SICK; return; }
  if (pet.hunger < 25)                                  { currentMood = MOOD_HUNGRY;  return; }
  if (pet.happiness > 80 && wifiStats.netCount > 8)     { currentMood = MOOD_EXCITED; return; }
  if (pet.happiness > 60 && wifiStats.netCount > 0)     { currentMood = MOOD_HAPPY;   return; }
  if (wifiStats.netCount == 0 && millis() - lastWifiScanTime > 30000) {
    currentMood = MOOD_BORED; return;
  }
  if (wifiStats.hiddenCount > 0 || wifiStats.openCount > 0) { currentMood = MOOD_CURIOUS; return; }
  currentMood = MOOD_CALM;
}

void updateEvolution() {
  // Total age in minutes. The original only looked at the minutes field,
  // which wraps at 60, so a pet could never reach ADULT/ELDER.
  unsigned long a = pet.ageMinutes + pet.ageHours * 60UL + pet.ageDays * 1440UL;
  int avg = (pet.hunger + pet.happiness + pet.health) / 3;

  Stage before = petStage;
  if (a >= 180 && avg > 40 && petStage < STAGE_ELDER)      { petStage = STAGE_ELDER; sndEvolve(); }
  else if (a >= 60 && avg > 45 && petStage < STAGE_ADULT)  { petStage = STAGE_ADULT; sndEvolve(); }
  else if (a >= 20 && avg > 35 && petStage < STAGE_TEEN)   { petStage = STAGE_TEEN;  sndEvolve(); }
  if (petStage != before) { backupForce = true; saveState(); }
}

// =====================================================================
//  Rest state machine (bedtime sleep)
// =====================================================================
void stepRest() {
  if (currentActivity != ACT_REST || restPhase == REST_NONE) return;
  unsigned long now = millis();

  switch (restPhase) {
    case REST_ENTER:
      if (now - lastRestAnimTime >= REST_ENTER_DELAY) {
        lastRestAnimTime = now;
        if (restFrameIndex > 0) restFrameIndex--;
        else {
          restFrameIndex   = 0;
          restPhase        = REST_DEEP;
          restPhaseStart   = now;
          restStatsApplied = false;
        }
      }
      break;

    case REST_DEEP: {
      // Asleep

      if (petBedtime || rockedSleep) break;   // sleeps until woken (see updateSleepSenses)

      if (!restStatsApplied && now - restPhaseStart > restDurationMs / 2) {
        pet.hunger    = constrain(pet.hunger - 1,     0, 100);
        pet.happiness = constrain(pet.happiness + 10, 0, 100);
        pet.health    = constrain(pet.health + 15,    0, 100);
        restStatsApplied = true;
      }

      if (now - restPhaseStart >= restDurationMs) {
        restPhase        = REST_WAKE;
        restPhaseStart   = now;
        lastRestAnimTime = now;
        sndRestEnd();
        ledsOff();
        restFrameIndex = 0;
      }
      break;
    }

    case REST_WAKE:
      if (now - lastRestAnimTime >= REST_WAKE_DELAY) {
        lastRestAnimTime = now;
        if (restFrameIndex < 4) restFrameIndex++;
        else {
          restFrameIndex  = 4;
          restPhase       = REST_NONE;
          currentActivity = ACT_NONE;
        }
      }
      break;

    default: break;
  }
}

// =====================================================================
//  WiFi-based activities (unchanged)
// =====================================================================
void resolveHunt() {
  int n = wifiStats.netCount;
  int hungerDelta = 0, happyDelta = 0, healthDelta = 0;

  if (n == 0) {
    happyDelta = -1;   // found nothing: a bit disappointed, no food lost (was -15/-10/-5)
    sndBadFeed();
  } else {
    hungerDelta = constrain(n * 2 + wifiStats.strongCount * 3, MIN_MEAL, 35);
    int varietyScore = wifiStats.hiddenCount * 2 + wifiStats.openCount;
    happyDelta = min(30, varietyScore * 3 + (wifiStats.avgRSSI + 100) / 3);
    if (wifiStats.avgRSSI > -75) healthDelta += 5;
    if (wifiStats.avgRSSI > -65) healthDelta += 5;
    if (wifiStats.strongCount > 5) healthDelta += 3;
    sndGoodFeed();
  }

  pet.hunger    = constrain(pet.hunger + hungerDelta,   0, 100);
  pet.happiness = constrain(pet.happiness + happyDelta, 0, 100);
  pet.health    = constrain(pet.health + healthDelta,   0, 100);

  hungerEffectActive  = true;
  hungerEffectFrame   = 0;
  lastHungerFrameTime = millis();
}

void resolveDiscover() {
  int n = wifiStats.netCount;
  int happyDelta = 0, hungerDelta = 0;

  if (n == 0) {
    happyDelta = -5;
    sndBadFeed();
  } else {
    int curiosity = wifiStats.hiddenCount * 4 + wifiStats.openCount * 3 + wifiStats.netCount;
    happyDelta  = min(35, curiosity / 2);   // exploring no longer costs food (was -5)
    sndDiscover();
  }

  pet.happiness = constrain(pet.happiness + happyDelta, 0, 100);
  pet.hunger    = constrain(pet.hunger + hungerDelta,   0, 100);
}

void startRest(unsigned long durationMs) {
  currentActivity  = ACT_REST;
  restPhase        = REST_ENTER;
  restFrameIndex   = 4;
  lastRestAnimTime = millis();
  restPhaseStart   = millis();
  restDurationMs   = durationMs;
  restStatsApplied = false;
  sndRestStart();
  ledsRest();
}

// =====================================================================
//  Autonomous decisions (unchanged)
// =====================================================================
void decideNextActivity() {
  if (currentActivity != ACT_NONE || restPhase != REST_NONE) return;
  if (showsRunning()) return;                           // hunts wait until the show is over

  unsigned long now = millis();
  if (now - lastDecisionTime < currentDecisionInterval) return;
  lastDecisionTime        = now;
  currentDecisionInterval = random(DECISION_INTERVAL_MIN, DECISION_INTERVAL_MAX);
  if (scanSaver) currentDecisionInterval *= 3;          // Saver: about one scan every 1.5-2 min
  if (runningHot) currentDecisionInterval *= 2;         // let the radio cool down

  int desireIdle = 10;
  int desireHunt = (100 - pet.hunger) + traitCuriosity / 2;
  if (pet.hunger < 50) desireHunt += 30;     // getting hungry: eat before exploring
  if (wifiStats.netCount == 0) desireHunt /= 2;

  // Upstream's explore desire grew without limit, so in busy WiFi areas the
  // pet explored forever and never ate or slept. Capped now.
  int envInterest = min(40, wifiStats.hiddenCount * 10 + wifiStats.openCount * 6 +
                            wifiStats.netCount * 2);
  int desireDisc = traitCuriosity / 2 + envInterest + random(0, 20);
  if (wifiStats.netCount == 0) desireDisc /= 2;

  int desireRest = (100 - pet.health) + traitStress / 2;
  if (pet.health < 50) desireRest += 30;     // feeling weak: nap
  if (pet.hunger < 20) desireRest -= 10;

  switch (currentMood) {
    case MOOD_HUNGRY:  desireHunt += 20; desireRest -= 10; break;
    case MOOD_CURIOUS: desireDisc += 15; break;
    case MOOD_SICK:    desireRest += 20; desireDisc -= 10; break;
    case MOOD_EXCITED: desireDisc += 10; desireHunt += 5;  break;
    case MOOD_BORED:   desireDisc += 10; desireHunt += 5;  break;
    default: break;
  }

  desireHunt = max(desireHunt, 0);
  desireDisc = max(desireDisc, 0);
  desireRest = max(desireRest, 0);

  int best = desireIdle;
  Activity chosen = ACT_NONE;
  if (desireHunt > best) { best = desireHunt; chosen = ACT_HUNT; }
  if (desireDisc > best) { best = desireDisc; chosen = ACT_DISCOVER; }

  if (chosen == ACT_NONE) return;

  if (chosen == ACT_HUNT || chosen == ACT_DISCOVER) {
    currentActivity = chosen;
    ledsWifi();
    startWifiScan();
  }
  // No more naps on its own: it only sleeps when you lay it down.
}

// =====================================================================
//  Reset
// =====================================================================
void resetPet(bool fullReset) {
  pet.hunger = pet.happiness = pet.health = 70;
  if (fullReset) pet.ageMinutes = pet.ageHours = pet.ageDays = 0;

  wifiStats        = WifiStats();
  lastWifiScanTime = 0;

  unsigned long now = millis();
  hungerTimer = happinessTimer = healthTimer = ageTimer = now;

  currentActivity    = ACT_NONE;
  restPhase          = REST_NONE;
  petBedtime         = false;
  rockedSleep        = false;
  hungerEffectActive = false;
  if (wifiScanInProgress) { WiFi.scanDelete(); WiFi.mode(WIFI_OFF); }
  wifiScanInProgress = false;
  ledsOff();
}

// Home opens the menu on a *click* (release) so that holding A can switch views.
// If A is still held down when we arrive home (e.g. pressed "Back" in a menu),
// its release must not count as a click there.
static bool swallowClickA = false;

static void goToScreen(Screen s) {
  if (s == SCREEN_HOME && M5.BtnA.isPressed()) swallowClickA = true;
  currentScreen = s;
  uiOnScreenChange(s);
}

// =====================================================================
//  Bedtime: lay the stick down flat and still -> pet sleeps until picked up
// =====================================================================
static void updateBedtime() {
  unsigned long now = millis();

  if (!petBedtime) {
    if (!lieDownSleep || !M5.Imu.isEnabled()) return;
    bool settled = lyingFlat && now - flatSince > LIE_DOWN_MS && now - lastMoveTime > LIE_DOWN_MS;
    if (!settled || currentScreen != SCREEN_HOME || now < noSleepUntil) return;
    if (rockedSleep) { rockedSleep = false; petBedtime = true; return; }   // rocked baby laid in bed
    if (currentActivity == ACT_HUNT || currentActivity == ACT_DISCOVER) return;   // let it finish

    if (currentActivity == ACT_REST && restPhase != REST_NONE) {
      if (restPhase == REST_WAKE) { restPhase = REST_ENTER; restFrameIndex = 4 - restFrameIndex; }
    } else {
      startRest(0);
    }
    petBedtime = true;
    return;
  }

  // In bed: wake up when picked up - tilted past ~20 deg or bumped - or if
  // the feature was turned off
  float tiltXY = sqrtf(imuX * imuX + imuY * imuY);
  bool  lifted = tiltXY > WAKE_XY_G || fabsf(imuZ) < 0.8f;
  if (!lifted && lieDownSleep) return;
  petBedtime = false;
  if (restPhase == REST_DEEP || restPhase == REST_ENTER) {
    if (restPhase == REST_ENTER) restFrameIndex = 4 - restFrameIndex;   // reverse smoothly
    else                         restFrameIndex = 0;
    restPhase        = REST_WAKE;
    restPhaseStart   = now;
    lastRestAnimTime = now;
    sndRestEnd();
    ledsOff();
  }
}

// =====================================================================
//  Senses -> behaviour: rocking to sleep, stirring, waking in shock,
//  noticing claps, listening, dancing, walking
// =====================================================================
static void wakeGently() {
  petBedtime = false; rockedSleep = false;
  if (restPhase == REST_DEEP || restPhase == REST_ENTER) {
    restFrameIndex   = restPhase == REST_ENTER ? 4 - restFrameIndex : 0;
    restPhase        = REST_WAKE;
    restPhaseStart   = millis();
    lastRestAnimTime = millis();
    sndRestEnd();
  }
}

static unsigned long shockSoundAt = 0;
static void wakeInShock() {
  unsigned long now = millis();
  petBedtime = false; rockedSleep = false;
  currentActivity = ACT_NONE;
  restPhase       = REST_NONE;
  startleUntil    = now + 2600;
  noSleepUntil    = now + 20000;
  radarHoldUntil  = now + 30000;      // no Bluetooth burst right on top of the scare
  lastInputTime   = now;              // screen comes on (gliding up) to show the scare
  pet.happiness   = max(0, pet.happiness - 2);
  shockSoundAt    = now + 300;        // yelp a moment later, once the screen is up
}

static void pokeHeard(unsigned long hitAt, unsigned long now);
static void updateSleepSenses() {
  unsigned long now = millis();
  bool asleep = currentActivity == ACT_REST && (restPhase == REST_DEEP || restPhase == REST_ENTER);
  bool bang   = sensesTakeBang();
  bool clap   = sensesTakeClap();
  bool called = sensesTakeDoubleClap();
  bool step   = sensesTakeStep();
  pokeHeard(sensesTakeKnock(), now);       // the mic catches taps the accelerometer misses
  if (called) lastInputTime = now;          // calling it always turns the screen on
  bool hardJolt = now - strongJoltTime < 250;
  bool softJolt = now - lastJoltTime < 250 && !hardJolt;

  if (asleep) {
    if (hardJolt || (bang && lastBangDb >= SHOCK_SOUND_DB)) { wakeInShock(); return; }
    if (called) {                            // clap-clap: wakes up for you, happily
      wakeGently();
      noSleepUntil = now + 20000;
      calledUntil  = now + 3800;
      return;
    }
    static unsigned long lastStir = 0;
    if ((softJolt || bang || clap || listening) && now - lastStir > 2500) { stirUntil = now + 1300; lastStir = now; }
    if (rockedSleep && now - rockedSince > ROCKED_NAP_MS) wakeGently();
    return;
  }

  // Awake
  if (currentScreen == SCREEN_HOME && currentActivity == ACT_NONE) {
    if (bang)      { startleUntil = now + 2200; noticeUntil = now + 3800; pet.happiness = max(0, pet.happiness - 1); sndShock(); }
    else if (clap) { startleUntil = now + 1100; noticeUntil = now + 2800; }   // "!?" then looks at you
    if (called) {                            // clap-clap: "you called?"
      static unsigned long lastCallReward = 0;
      calledUntil = now + 2600; startleUntil = 0; noticeUntil = 0;
      if (now - lastCallReward > 30000) { pet.happiness = min(100, pet.happiness + 1); lastCallReward = now; }
      sndStart(9);
    }

    // Rocked until drowsy -> falls asleep in your arms
    if (rockDrowsy >= 1.0f && now >= noSleepUntil) {
      rockDrowsy  = 0;
      rockedSleep = true;
      rockedSince = now;
      startRest(0);
      restFrameIndex = 0;        // eyes were already heavy: go straight to closed, no re-opening
    }
  }

  // Walking with it: rewards every 100 steps, a party at the daily goal
  if (step) {
    static uint32_t lastRewardAt = 0;
    if (stepsToday / STEP_REWARD_EVERY != lastRewardAt / STEP_REWARD_EVERY) {
      pet.happiness = min(100, pet.happiness + 1);
      pet.health    = min(100, pet.health + 1);
    }
    if (lastRewardAt < STEP_GOAL && stepsToday >= STEP_GOAL) { stepGoalUntil = now + 5000; playEffectUntil = now + 1500; sndPlay(); }
    lastRewardAt = stepsToday;
  }

  // Being talked to / hearing music cheers it up a little
  static unsigned long lastSocial = 0;
  if ((listening || dancing) && now - lastSocial > 30000) {
    lastSocial = now;
    pet.happiness = min(100, pet.happiness + (dancing ? 2 : 1));
  }
}

// =====================================================================
//  Logic tick (every 100 ms, unchanged rules)
// =====================================================================
#ifdef SILA_DEV
static uint32_t devWarp = 1;              // serial test: stats change this many times faster
#define DECAY(ms) ((ms) / devWarp)
#else
#define DECAY(ms) (ms)
#endif

void logicTick() {
  unsigned long now = millis();

  bool deepBedtime = (petBedtime || rockedSleep) && restPhase == REST_DEEP;

  if (now - hungerTimer >= DECAY(deepBedtime ? HUNGER_DECAY_MS * 3 : HUNGER_DECAY_MS)) {   // sleeping burns less
    if (powerState == PWR_BATTERY) pet.hunger = max(0, pet.hunger - 1);   // plugged in = eating electricity
    hungerTimer = now;
  }
  if (now - happinessTimer >= DECAY(HAPPY_DECAY_MS)) {
    bool lonely = wifiStats.netCount == 0 && (now - lastWifiScanTime) > 30000;
    if (!deepBedtime) pet.happiness = max(0, pet.happiness - (lonely ? 2 : 1));
    happinessTimer = now;
  }
  if (now - healthTimer >= DECAY(HEALTH_DECAY_MS)) {
    // Looked after = slowly heals; neglected = gets sick. It used to lose health
    // all the time, so even a full, happy pet on a desk got sick in a few hours.
    bool neglected = pet.hunger < 20 || pet.happiness < 20;
    bool wellKept  = pet.hunger >= 50 && pet.happiness >= 50;
    if (!deepBedtime) pet.health = constrain(pet.health + (wellKept ? 1 : neglected ? -2 : -1), 0, 100);
    healthTimer = now;
  }

  if (deepBedtime) {                                    // a good sleep heals: +1 health / min
    if (now - bedHealTimer >= DECAY(BED_HEAL_MS)) { pet.health = min(100, pet.health + 1); bedHealTimer = now; }
  } else {
    bedHealTimer = now;
  }

  if (now - ageTimer >= 60000) {
    pet.ageMinutes++;
    if (pet.ageMinutes >= 60) { pet.ageMinutes -= 60; pet.ageHours++; }
    if (pet.ageHours >= 24)   { pet.ageHours -= 24;   pet.ageDays++;  }
    ageTimer = now;
  }

  if (hungerEffectActive && now - lastHungerFrameTime >= HUNGER_EFFECT_DELAY) {
    lastHungerFrameTime = now;
    hungerEffectFrame++;
    if (hungerEffectFrame >= HUNGER_FRAME_COUNT) {
      hungerEffectActive = false;
      hungerEffectFrame  = 0;
      ledsOff();
    }
  }

  if (currentActivity == ACT_HUNT || currentActivity == ACT_DISCOVER) {
    if (checkWifiScanDone()) {
      if (scanFailed)                       { }
      else if (currentActivity == ACT_HUNT) resolveHunt();
      else                                  resolveDiscover();
      currentActivity = ACT_NONE;
      if (!hungerEffectActive) ledsOff();
    }
  }

  updateBedtime();
  updateSleepSenses();
  stepRest();
  updateMood();
  updateEvolution();

  if (pet.hunger <= 0 && pet.happiness <= 0 && pet.health <= 0 &&
      currentScreen != SCREEN_GAMEOVER) {
    goToScreen(SCREEN_GAMEOVER);
    currentActivity = ACT_NONE;
    restPhase       = REST_NONE;
    ledsSad();
  }

  if (now - lastSaveTime >= autoSaveMs) {
    lastSaveTime = now;
    saveState();
  }

  if (currentScreen == SCREEN_HOME && currentActivity == ACT_NONE && restPhase == REST_NONE) {
    decideNextActivity();
  }
}

// Hold A on the home screen: pat it on the head
static void tryPat() {
  if (currentScreen != SCREEN_HOME) return;
  if (currentActivity != ACT_NONE || restPhase != REST_NONE) return;
  unsigned long now = millis();
  if (lastPatTime != 0 && now - lastPatTime < PAT_COOLDOWN_MS) { sndClick(); return; }
  lastPatTime = now;
  pet.happiness = constrain(pet.happiness + 4, 0, 100);
  playEffectUntil = now + PLAY_EFFECT_MS;
  sndPlay();
}

// =====================================================================
//  Poke and free fall (fed by every IMU sample, ~66 Hz)
// =====================================================================
static const float    TAP_G        = 0.40f;  // a sudden bump this big out of stillness = poke
static const float    TAP_QUIET_G  = 0.15f;  // "still" before it...
static const uint32_t TAP_QUIET_MS = 150;    // ...for this long (fast pokes ~3/s still count)
static const uint32_t TAP_CONFIRM  = 250;    // no more jolts may follow within this (else shake/handling)
static const float    FREEFALL_G   = 0.35f;  // weightless...
static const int      FREEFALL_N   = 3;      // ...for this many samples (~45 ms, a ~1 cm drop)
static const float    HARD_LAND_G  = 3.5f;   // landing harder than this = ouch (hand catch ~2.8 g, pillow ~1.6 g: whee)

static bool          fallActive  = false;
static unsigned long fallEndedAt = 0;

static bool petAwakeOnHome() {
  return currentScreen == SCREEN_HOME && restPhase == REST_NONE && currentActivity != ACT_REST;
}

static void onPoke(unsigned long now) {
  static unsigned long pokes[6] = {0};
  static int idx = 0;
  static unsigned long lastReward = 0, lastPenalty = 0;
  lastInputTime = now;
  if (!petAwakeOnHome()) return;                 // asleep: the bump already makes it stir
  pokes[idx] = now; idx = (idx + 1) % 6;
  int n = 0;
  for (unsigned long t : pokes) if (t && now - t < 8000) n++;
  if (n <= 3) {                                  // giggles
    pokeLevel = 1; giggleUntil = now + 1100; grumpyUntil = 0;
    if (now - lastReward > 20000) { pet.happiness = min(100, pet.happiness + 1); lastReward = now; }
    sndStart(10);
  } else if (n <= 5) {                           // "hey!"
    pokeLevel = 2; grumpyUntil = now + 1400; giggleUntil = 0;
    sndStart(11);
  } else {                                       // "stop poking!"
    pokeLevel = 3; grumpyUntil = now + 2800; giggleUntil = 0;
    if (now - lastPenalty > 5000) { pet.happiness = max(0, pet.happiness - 1); lastPenalty = now; }
    sndStart(11);
  }
}

static unsigned long lastPokeHit = 0;           // when the last counted poke happened

// A tap the mic heard while the stick moved slightly (from senses.cpp)
static void pokeHeard(unsigned long hitAt, unsigned long now) {
  if (!hitAt || fallActive || now - fallEndedAt < 1500) return;
  if (labs((long)(hitAt - lastPokeHit)) <= 300) return;   // the accelerometer already counted it
  lastPokeHit = hitAt;
  Serial.printf("IMU-EV POKE (heard)\n");
  onPoke(now);
}

static void updatePoke(float d, unsigned long now) {
  static unsigned long busyAt = 0, candAt = 0;
  static int after = 0;                          // bumps after the poke (handling / shaking)
  if (fallActive || now - fallEndedAt < 1500) { candAt = 0; busyAt = now; return; }
  if (candAt) {
    if (now - candAt > 80 && d >= 0.3f) after++;
    if (now - candAt > 80 && (d >= 0.7f || after >= 3)) {   // more jolts: shaking or handling it
      Serial.printf("IMU-EV poke dropped (%.2fg, %d after)\n", d, after);
      candAt = 0;
    } else if (now - candAt >= TAP_CONFIRM) {
      if (labs((long)(candAt - lastPokeHit)) > 300) {   // the mic may have reported it already
        lastPokeHit = candAt;
        Serial.printf("IMU-EV POKE\n");
        onPoke(now);
      }
      candAt = 0;
    }
  } else if (d >= TAP_G && now - busyAt > TAP_QUIET_MS) {
    candAt = now; after = 0;                     // sharp bump straight out of stillness
    Serial.printf("IMU-EV poke? %.2fg\n", d);
  }
  if (d > TAP_QUIET_G) busyAt = now;
}

static void updateFall(float mag, unsigned long now) {
  static int ff = 0;
  static unsigned long fallAt = 0, impactAt = 0;
  static float peak = 0;
  ff = mag < FREEFALL_G ? ff + 1 : 0;

  if (!fallActive) {
    if (ff < FREEFALL_N || now - fallEndedAt < 1500 || !hasHatchedOnce ||
        currentScreen == SCREEN_HATCH || currentScreen == SCREEN_BOOT) return;
    fallActive = true; fallAt = now; impactAt = 0; peak = 0;
    Serial.printf("IMU-EV FALL\n");
    lastInputTime = now;
    if (currentActivity == ACT_REST || restPhase != REST_NONE) {   // falling out of bed!
      petBedtime = false; rockedSleep = false;
      currentActivity = ACT_NONE; restPhase = REST_NONE;
      noSleepUntil = now + 20000;
    }
    fallUntil = now + 3000;
    sndStart(12);                                // AAAAH
    return;
  }

  // Falling: the first real push back (> 1.3 g) is the landing or the catch.
  if (!impactAt && mag > 1.3f) impactAt = now;
  if (impactAt) peak = max(peak, mag);
  bool landed  = impactAt && now - impactAt > 150;
  bool timeout = !impactAt && now - fallAt > 3000;
  if (!landed && !timeout) return;

  fallActive = false; fallEndedAt = now; fallUntil = 0;
  Serial.printf("IMU-EV LAND peak=%.2fg after %lums\n", peak, impactAt ? impactAt - fallAt : 0UL);
  if (peak >= HARD_LAND_G) {                     // bonk
    ouchUntil = now + 900; dizzyUntil = now + 2800;      // "ow!", then dizzy
    pet.happiness = max(0, pet.happiness - 2);
    pet.health    = max(0, pet.health - 1);
    sndStart(14);
  } else {                                       // thrown up and caught: fun!
    static unsigned long lastWheeReward = 0;
    wheeUntil = now + 2200;
    if (now - lastWheeReward > 20000) { pet.happiness = min(100, pet.happiness + 2); lastWheeReward = now; }
    sndStart(13);
  }
}

// Game tilt: the right edge of the screen tipping down = positive.
// Uses the same axes as applyRotation (base 1: screen right = -Y).
float tiltRight() {
  return constrain(baseRot == 1 ? imuY : -imuY, -1.0f, 1.0f);
}

// A "shake" = several quick jolts in a short window (a back-and-forth flick
// of the wrist). One bump from walking or dropping it in a bag is a single
// jolt, so it no longer counts, even though each jolt can now be gentler.
static bool readShake() {
  static unsigned long peakTimes[SHAKE_PEAKS_NEEDED] = {0};
  static int           peakIdx  = 0;
  static unsigned long lastPeak = 0;

  unsigned long now = millis();
  if (now - lastImuRead < 15) return false;
  lastImuRead = now;

  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
  float mag = sqrtf(ax * ax + ay * ay + az * az);

  // Lying flat (screen up or down) and still? Used for bedtime.
  if (fabsf(mag - 1.0f) > STILL_TOLERANCE_G) lastMoveTime = now;
  if (fabsf(mag - 1.0f) > WAKE_JOLT_G)  lastJoltTime = now;
  if (fabsf(mag - 1.0f) > SHOCK_JOLT_G) strongJoltTime = now;
  sensesImu(ax, ay, az, mag, now);
  bool flat = fabsf(az) > 0.9f && fabsf(ax) < FLAT_XY_G && fabsf(ay) < FLAT_XY_G;
  if (flat && !lyingFlat) flatSince = now;
  lyingFlat = flat;
  imuX = ax; imuY = ay; imuZ = az;

  // Movement = accel minus a slow low-pass (gravity). Expressed in screen space
  // of the landscape panel so the face can sway with it.
  static float lpX = 0, lpY = 0, lpZ = 1;
  lpX += (ax - lpX) * 0.06f; lpY += (ay - lpY) * 0.06f; lpZ += (az - lpZ) * 0.06f;
  float dX = ax - lpX, dY = ay - lpY;
  // base 1: screen right = -Y, screen up = +X ; base 3: right = +Y, up = -X
  float mRight = baseRot == 1 ? -dY : dY;
  float mUp    = baseRot == 1 ?  dX : -dX;
  motionX = mRight;
  motionY = -mUp;
  imuDebugX = ax; imuDebugY = ay;

  updateFall(mag, now);
  updatePoke(fabsf(mag - 1.0f), now);
  if (fallActive || now - fallEndedAt < 1000) return false;   // a fall is not a shake

  if (fabsf(mag - 1.0f) < SHAKE_THRESHOLD_G) return false;
  if (now - lastPeak < SHAKE_PEAK_GAP_MS) return false;   // same jolt, still ringing
  lastPeak = now;

  peakTimes[peakIdx] = now;
  peakIdx = (peakIdx + 1) % SHAKE_PEAKS_NEEDED;
  unsigned long oldest = peakTimes[peakIdx];               // next slot = oldest peak
  if (oldest == 0 || now - oldest > SHAKE_WINDOW_MS) return false;
  if (now - lastShakeTime < SHAKE_DEBOUNCE_MS) return false;

  lastShakeTime = now;
  for (auto &t : peakTimes) t = 0;                          // need a fresh shake next time
  return true;
}

// =====================================================================
//  Power: charging state, battery % and temperature
//
//  The charger's status pin flickers when the battery is nearly full or when
//  a WiFi scan briefly draws more than the charger supplies. So: USB voltage
//  decides "plugged in", the status pin is smoothed over ~10 s, and the shown
//  percentage only moves in the direction that makes sense.
//
//  Battery %: M5Unified maps 3.30-4.15 V in a straight line, but a LiPo sits
//  on a long plateau around 3.7-3.9 V and drops off a cliff below 3.65 V, so
//  the straight line said "39%" for a nearly empty cell. Now: a LiPo curve, the
//  voltage averaged over ~30 s (ignoring WiFi scans, which sag it), and the
//  charger's voltage boost taken off while charging.
// =====================================================================
int batteryMv = 0;                         // averaged battery voltage (System page)

static int lipoPercent(int mv) {
  static const int16_t MV[]  = { 3450, 3600, 3670, 3710, 3750, 3780, 3810, 3860, 3920, 4000, 4100, 4180 };
  static const int8_t  PCT[] = {    0,    5,   10,   20,   30,   40,   50,   60,   70,   80,   90,  100 };
  if (mv <= MV[0]) return 0;
  for (int i = 1; i < 12; i++)
    if (mv < MV[i]) return PCT[i - 1] + (PCT[i] - PCT[i - 1]) * (mv - MV[i - 1]) / (MV[i] - MV[i - 1]);
  return 100;
}

static const int CHARGE_BOOST_MV = 25;     // charging lifts the reading this much (measured on the stick)
static const int FULL_MV         = 4100;   // charger done AND at least this = full

static void updatePower() {
  static unsigned long last = 0;
  static uint16_t chgHistory = 0;          // 1 bit per second, newest in bit 0
  static int samples = 0;
  static float mvAvg = 0;
  unsigned long now = millis();
  if (last != 0 && now - last < 1000) return;
  last = now;

  int  vbus    = M5.Power.getVBUSVoltage();             // mV
  bool plugged = vbus > 4300;
  static bool wasPlugged = false;
  bool plugChanged = plugged != wasPlugged;
  if (plugChanged) { earsIgnore(1500); wasPlugged = plugged; chgHistory = 0; samples = 0; }
  // Plugged in: it eats electricity (a snack, and no hunger loss while charging)
  if (plugChanged && plugged && now > 5000 && hasHatchedOnce && currentScreen != SCREEN_HATCH) {
    zapUntil = now + 2600;
    pet.hunger = min(100, pet.hunger + 5);
    lastInputTime = now;
    if (restPhase == REST_NONE) sndStart(15);
  }   // the plug "clacks"; judge charging afresh
  bool chgPin  = M5.Power.isCharging() == m5::Power_Class::is_charging;
  chgHistory = (chgHistory << 1) | (chgPin ? 1 : 0);
  if (samples < 16) samples++;

  int recent = __builtin_popcount(chgHistory & 0x3FF);  // last 10 s

  int mv = M5.Power.getBatteryVoltage();
  if (mv > 2500) {
    if (mvAvg == 0 || plugChanged) mvAvg = mv;              // first reading / plug: start fresh
    else if (!wifiScanInProgress) mvAvg += (mv - mvAvg) / 30.0f;
    batteryMv = (int)mvAvg;
  }

  // "Full" needs a quiet charge pin AND a full cell's voltage: in v3.6 the pin
  // flickered (5V boost loop) and a 3.65 V battery was called full = 100%.
  if (!plugged)                                      powerState = PWR_BATTERY;
  else if (recent < 2 && samples >= 10 && batteryMv >= FULL_MV) powerState = PWR_FULL;
  else if (recent >= 2 || samples >= 10)             powerState = PWR_CHARGING;

  if (mv > 2500) {
    int restMv = batteryMv - (powerState == PWR_CHARGING ? CHARGE_BOOST_MV : 0);
    int raw = lipoPercent(restMv);

    if (batteryShown < 0 || plugChanged) batteryShown = raw;
    else if (powerState == PWR_BATTERY) {
      // on battery it only goes down (voltage sags under load and recovers)
      if (raw < batteryShown) batteryShown = raw;
      else if (raw > batteryShown + 8) batteryShown = raw;  // big jump = real recalibration
    } else {
      // plugged in it only goes up; the charger finishing means full
      if (raw > batteryShown) batteryShown = raw;
      if (powerState == PWR_FULL) batteryShown = 100;
    }
  }

  // Power log on USB serial (115200), one line a second, for tuning.
  {
    bool chgEn = false; M5.Power.M5pm1.getBatteryCharge(&chgEn);
    Serial.printf("PWR t=%lu vbus=%d vbat=%d avg=%d pin=%d chgEn=%d boost=%d src=0x%02x state=%d shown=%d\n",
                  now / 1000, vbus, mv, batteryMv, chgPin ? 1 : 0, chgEn ? 1 : 0,
                  M5.Power.getExtOutput() ? 1 : 0, (int)M5.Power.M5pm1.getPowerSource(),
                  (int)powerState, batteryShown);
  }

  float t = temperatureRead();
  chipTempC = chipTempC == 0 ? t : chipTempC + (t - chipTempC) * 0.2f;
  if (!runningHot && chipTempC > 72) runningHot = true;    // hysteresis
  if (runningHot && chipTempC < 64)  runningHot = false;
}

// =====================================================================
//  Screen auto-sleep
// =====================================================================
static void setScreenState(ScreenState s) {
  if (s == screenState) return;
  screenState  = s;
  screenAsleep = (s != SCR_ON);
  applyTftBrightness();
}

static void wakeScreen() { setScreenState(SCR_ON); }

// What the screen should be doing right now (home screen only)
static ScreenState wantedScreenState() {
  if (currentScreen != SCREEN_HOME) return SCR_ON;
  unsigned long idle = millis() - lastInputTime;   // read fresh, never wraps backwards
  if (idle >= 0x80000000UL) return SCR_ON;

  bool petSleeping = currentActivity == ACT_REST && restPhase == REST_DEEP;
  if (idleMode != IDLE_ON && idle > SCREEN_SLEEP_MS)
    return idleMode == IDLE_BLACK ? SCR_OFF : SCR_DIM;
  if (petSleeping && idle > 5000) return SCR_DIM;     // dim but keep showing it asleep
  return SCR_ON;
}

// =====================================================================
//  Input handling: 2 buttons + shake
// =====================================================================
void handleInput() {
  bool ok      = M5.BtnA.wasPressed();
  bool okClick = M5.BtnA.wasClicked();
  bool okHold  = M5.BtnA.wasHold();
  if (okClick && swallowClickA)            { okClick = false; swallowClickA = false; }
  else if (!M5.BtnA.isPressed() && !okClick) swallowClickA = false;   // never let it go stale
  // Button clicks are loud right next to the microphone: ears ignore them
  if (M5.BtnA.wasPressed() || M5.BtnA.wasReleased() || M5.BtnB.wasPressed() || M5.BtnB.wasReleased())
    earsIgnore(500);
  bool next  = M5.BtnB.wasClicked();   // short press (fires on release)
  bool back  = M5.BtnB.wasHold();      // long press
  bool shake = M5.Imu.isEnabled() && readShake();

  unsigned long now = millis();

  if (ok || next || back || shake) {
    lastInputTime = now;
    if (screenAsleep) {         // first input only wakes the screen
      wakeScreen();
      if (ok) swallowClickA = true;
      return;
    }
  }

  // ---- BOOT (waking-up animation): any button skips it ----
  if (currentScreen == SCREEN_BOOT) {
    if (ok || next) goToScreen(SCREEN_HOME);
    return;
  }

  // ---- HATCH: A hatches the egg (shake works too) ----
  if (currentScreen == SCREEN_HATCH) {
    if ((ok || shake) && !hasHatchedOnce) {
      sndClick();
      hatchTriggered = true;
    }
    return;
  }

  // ---- HOME ----
  if (currentScreen == SCREEN_HOME && rockedSleep && (ok || next)) {
    if (ok) swallowClickA = true;
    wakeGently();
    return;
  }
  if (currentScreen == SCREEN_HOME) {
    if (okHold) {               // hold A: pat it
      swallowClickA = true;     // releasing after a hold must not open the menu
      tryPat();
      return;
    }
    if (okClick) { sndClick(); mainMenuIndex = 0; goToScreen(SCREEN_MENU); return; }
    if (next)  { sndClick(); goToScreen(SCREEN_VITALS); return; }
    return;
  }

  // ---- Quick info pages: B cycles Vitals -> Pet Info -> WiFi -> Home ----
  if (currentScreen == SCREEN_VITALS || currentScreen == SCREEN_WALKS || currentScreen == SCREEN_PET_STATUS ||
      currentScreen == SCREEN_ENVIRONMENT || currentScreen == SCREEN_SYSINFO ||
      currentScreen == SCREEN_DIAGNOSTICS) {
    if (next) {
      sndClick();
      switch (currentScreen) {
        case SCREEN_VITALS:      goToScreen(SCREEN_WALKS);       break;
        case SCREEN_WALKS:       goToScreen(SCREEN_PET_STATUS);  break;
        case SCREEN_PET_STATUS:  goToScreen(SCREEN_ENVIRONMENT); break;
        default:                 goToScreen(SCREEN_HOME);        break;
      }
      return;
    }
    if (ok || back) { sndClick(); goToScreen(SCREEN_HOME); }
    return;
  }

  // ---- MAIN MENU ----
  if (currentScreen == SCREEN_MENU) {
    if (back) { sndClick(); goToScreen(SCREEN_HOME); return; }
    if (next) { sndClick(); mainMenuIndex = (mainMenuIndex + 1) % MAIN_MENU_COUNT; }
    if (ok) {
      sndClick();
      switch (mainMenuIndex) {
        case 0: goToScreen(SCREEN_VITALS);      break;
        case 1: gameEnter(); goToScreen(SCREEN_GAME); break;
        case 2: goToScreen(SCREEN_WALKS);       break;
        case 3: goToScreen(SCREEN_PET_STATUS);  break;
        case 4: goToScreen(SCREEN_ENVIRONMENT); break;
        case 5: controlsIndex = 0;     goToScreen(SCREEN_CONTROLS); break;
        case 6: settingsMenuIndex = 0; goToScreen(SCREEN_SETTINGS); break;
        case 7: goToScreen(SCREEN_SYSINFO);     break;
        case 8: goToScreen(SCREEN_HOME);        break;
      }
    }
    return;
  }

  // ---- CATCH (tilt game): A start / pause, hold B back ----
  if (currentScreen == SCREEN_GAME) {
    if (back) { sndClick(); saveState(); goToScreen(SCREEN_MENU); return; }
    if (ok) gameButtonA();
    return;
  }

  // ---- CONTROLS ----
  if (currentScreen == SCREEN_CONTROLS) {
    if (back) { sndClick(); goToScreen(SCREEN_MENU); return; }
    if (next) { sndClick(); controlsIndex = (controlsIndex + 1) % CONTROLS_COUNT; }
    if (ok) {
      switch (controlsIndex) {
        case 0: autoRotate = !autoRotate; break;
        case 1: tftBrightnessIndex = (tftBrightnessIndex + 1) % 3; applyTftBrightness(); break;
        case 2: volumeIndex = (volumeIndex + 1) % 3; applyVolume(); break;
        case 3:
          soundMode = (soundMode + 1) % 3;           // Key -> All -> Off -> Key
          if (soundMode == SOUND_OFF) M5.Speaker.stop();
          break;
        case 4: screenFlipped = !screenFlipped; break;
        case 5: sndClick(); goToScreen(SCREEN_MENU); return;
      }
      sndClick();   // after the change so you hear the new volume / sound state
      saveState();
    }
    return;
  }

  // ---- SETTINGS ----
  if (currentScreen == SCREEN_SETTINGS) {
    if (back) { sndClick(); goToScreen(SCREEN_MENU); return; }
    if (next) { sndClick(); settingsMenuIndex = (settingsMenuIndex + 1) % SETTINGS_COUNT; }
    if (ok) {
      sndClick();
      switch (settingsMenuIndex) {
        case 0: idleMode = (idleMode + 1) % 3; break;      // Dim -> Black -> Always on
        case 1: idleShows = (idleShows + 1) % 3; break;    // Lively -> Calm -> Off
        case 2: lieDownSleep = !lieDownSleep; break;
        case 3: earsEnabled  = !earsEnabled;  break;
        case 4: radarEnabled = !radarEnabled; break;
        case 5: scanSaver = !scanSaver; break;
        case 6:
          autoSaveMs = (autoSaveMs == 15000) ? 30000 : (autoSaveMs == 30000) ? 60000 : 15000;
          break;
        case 7: resetPet(false); break;
        case 8:
          resetPet(true);
          petStage       = STAGE_BABY;
          hasHatchedOnce = false;
          backupForce = true;
          saveState();
          goToScreen(SCREEN_HATCH);
          return;
        case 9: goToScreen(SCREEN_MENU); return;
      }
      saveState();
    }
    return;
  }

  // ---- GAME OVER ----
  if (currentScreen == SCREEN_GAMEOVER) {
    if (ok) {
      sndClick();
      resetPet(true);
      petStage       = STAGE_BABY;
      hasHatchedOnce = false;
      backupForce = true;
          saveState();
      goToScreen(SCREEN_HATCH);
    }
    return;
  }
}

// =====================================================================
//  setup & loop
// =====================================================================
void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  // M5Unified turns the 5V boost (Grove port) on by default. On the StickS3 it
  // shares the USB 5V line, so the battery fed the charger: the charge pin
  // flickered on/off, the battery never charged and drained even when idle.
  cfg.output_power = false;
  M5.begin(cfg);
  M5.Power.setExtOutput(false);
  M5.Power.setBatteryCharge(true);

  randomSeed(esp_random());
  setCpuFrequencyMhz(160);          // plenty for 30 fps; runs cooler than 240 MHz

  prefs.begin("tamafi_s3", false);  // old key kept on purpose: your pet survives the rename
  loadState();

  applyTftBrightness();
  applyVolume();
  M5.Speaker.end();                 // amp off until a sound plays (no charger hiss)

  fb.setColorDepth(16);
  fb.setPsram(false);              // 240x135x2 = 64 KB, fits in internal RAM (faster)
  if (!fb.createSprite(SCR_W, SCR_H)) {
    fb.setPsram(true);
    fb.createSprite(SCR_W, SCR_H);
  }
  faceRot = screenFlipped ? 3 : 1;
  applyRotation();

  // A started-but-unconnected WiFi radio keeps listening (~70-100 mA, most of
  // the battery). It is only switched on for each scan now.
  WiFi.mode(WIFI_OFF);

  unsigned long now = millis();
  hungerTimer = happinessTimer = healthTimer = ageTimer = now;
  lastLogicTick = lastSaveTime = lastDecisionTime = now;
  lastRestAnimTime = lastHungerFrameTime = lastInputTime = now;

  // First ever boot: straight into hatching. Afterwards: a waking-up animation.
  currentScreen = hasHatchedOnce ? SCREEN_BOOT : SCREEN_HATCH;
  uiInit();
  uiOnScreenChange(currentScreen);

  // Remember why it last restarted if that wasn't a normal power-on / flash,
  // so a crash shows up on the System page instead of being a mystery.
  esp_reset_reason_t rr = esp_reset_reason();
  const char* why = rr == ESP_RST_BROWNOUT ? "low power" : rr == ESP_RST_PANIC ? "crash"
                  : (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT) ? "froze" : nullptr;
  if (why) {
    prefs.putString("rstWhy", why);
    prefs.putUInt("rstN", prefs.getUInt("rstN", 0) + 1);
  }
  String w = prefs.getString("rstWhy", "");
  strncpy(lastResetWhy, w.c_str(), sizeof(lastResetWhy) - 1);
  lastResetCount = prefs.getUInt("rstN", 0);

  esp_task_wdt_init(10, true);        // loop stuck for 10 s -> reboot (pet is saved)
  esp_task_wdt_add(NULL);

  Serial.printf("Sila ready. Board id: %d, IMU: %s\n",
                (int)M5.getBoard(), M5.Imu.isEnabled() ? "ok" : "missing");
}

// One line every 5 s: the pet's stats, and how long drawing a frame takes.
static void logStats(unsigned long now, unsigned long drawMs) {
  static unsigned long lastLog = 0, sum = 0, worst = 0, frames = 0;
  sum += drawMs; frames++;
  if (drawMs > worst) worst = drawMs;
  if (now - lastLog < 5000) return;
  Serial.printf("STAT hunger=%d happy=%d health=%d mood=%d nets=%d rssi=%d shows=%d show=%d draw=%lu/%lums fps=%lu heap=%u\n",
                pet.hunger, pet.happiness, pet.health, (int)currentMood, wifiStats.netCount, wifiStats.avgRSSI,
                (int)idleShows, showsRunning() ? 1 : 0, sum / max(1UL, frames), worst,
                frames * 1000 / max(1UL, now - lastLog), (unsigned)ESP.getFreeHeap());
  lastLog = now; sum = worst = frames = 0;
}

// Letter + number + ';' commands. Returns true if the byte was used.
static bool serialCommand(int ch) {
  static char cmd = 0;
  static int value = 0;
  static bool negative = false;
  if (!cmd) {
    bool known = ch == 'P';
#ifdef SILA_DEV
    known = known || ch == 'H' || ch == 'J' || ch == 'K' || ch == 'W' || ch == 'G' || ch == 'A' || ch == 'U';
#endif
    if (!known) return false;
    cmd = (char)ch; value = 0; negative = false;
    return true;
  }
  if (ch == '-') { negative = true; return true; }
  if (ch >= '0' && ch <= '9') { value = value * 10 + (ch - '0'); return true; }
  if (negative) value = -value;
  char c = cmd;
  cmd = 0;
  if (c == 'P') showsStart((ShowId)value);
#ifdef SILA_DEV
  if (c == 'H') pet.hunger = value;
  if (c == 'J') pet.happiness = value;
  if (c == 'K') pet.health = value;
  if (c == 'W') devWarp = max(1, value);
  if (c == 'G') devFaceStage = value;
  if (c == 'A') devFaceAngle = value;
  if (c == 'U') { if (value < 0) goToScreen(SCREEN_HOME); else { settingsMenuIndex = value; goToScreen(SCREEN_SETTINGS); } }
#endif
  return true;
}

void loop() {
  M5.update();
  unsigned long now = millis();

  esp_task_wdt_reset();               // still alive (a hang reboots instead of freezing)
  if (shockSoundAt && now >= shockSoundAt) { shockSoundAt = 0; sndShock(); }
  sndUpdate();
  speakerIdleCheck();
  brightnessTick();

  if (now - lastLogicTick >= 100) {
    lastLogicTick = now;
    if (currentScreen != SCREEN_BOOT && currentScreen != SCREEN_HATCH) logicTick();
  }

  handleInput();

  updatePower();
  sensesUpdate();

  // Dimming never stops the pet: it keeps living, scanning and aging.
  setScreenState(wantedScreenState());

  // Frame pacing: 30 fps normally, slower when dimmed or hot. Between frames
  // the CPU idles instead of redrawing flat out (this was the main heat source).
  static unsigned long lastFrame = 0;
  uint32_t period = screenState == SCR_DIM ? 80 : runningHot ? 66 : 33;
  if (screenState == SCR_OFF) {
    delay(20);
  } else if (now - lastFrame >= period) {
    lastFrame = now;
    applyRotation();
    unsigned long drawStart = millis();
    uiDrawScreen(currentScreen, mainMenuIndex, controlsIndex, settingsMenuIndex);
    logStats(now, millis() - drawStart);
    // USB serial commands. 'S' = screenshot, used for README GIFs and for checking shows.
    // Reply: "SHOT w h bytes\n" + run-length data (count byte, 2 pixel bytes), mostly black so small.
    // 'P<n>;' = play idle show n.
    while (Serial.available()) {
      int ch = Serial.read();
      if (serialCommand(ch)) continue;
      if (ch != 'S') continue;
      const uint16_t* px = (const uint16_t*)fb.getBuffer();
      int total = fb.width() * fb.height(), n = 0, i = 0;
      static uint8_t* shot = (uint8_t*)ps_malloc(3 * SCR_W * SCR_H);   // worst case, every pixel different
      if (!shot) continue;
      while (i < total) {
        uint16_t v = px[i]; int run = 1;
        while (i + run < total && run < 255 && px[i + run] == v) run++;
        shot[n++] = run; shot[n++] = v & 0xFF; shot[n++] = v >> 8;
        i += run;
      }
      Serial.printf("SHOT %d %d %d\n", fb.width(), fb.height(), n);
      Serial.write(shot, n);
      Serial.println();
    }
  } else {
    delay(2);
  }
}
