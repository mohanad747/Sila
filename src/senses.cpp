// Sila - senses: ears (microphone), steps, rocking, people radar (BLE).
//
// Everything here only *measures*. It raises simple flags and events that
// main.cpp (pet logic) and face.cpp (expressions) react to.
//
// Privacy: the microphone is only used for loudness and rhythm, computed on
// the stick 50 times a second. No audio is stored or sent anywhere.
// The radar only counts nearby Bluetooth devices; nothing is connected to.

#include <Arduino.h>
#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <math.h>
#include "ui.h"
#include "shows.h"

// ---------------- Settings / shared state ----------------
bool earsEnabled  = true;
bool radarEnabled = true;

float micLevelDb = 0, micFloorDb = 0;
bool  listening = false, dancing = false;
uint32_t beatPeriodMs = 500;
unsigned long lastBeatTime = 0;

bool  walking = false;
unsigned long lastStepTime = 0;
uint32_t stepsToday = 0, stepsTotal = 0, stepsBest = 0;
uint16_t stepsDayIdx = 0;

float rockDrowsy = 0;
bool  rocking = false;

int   peopleNearby = -1;              // -1 = no scan yet
const char* peopleTopKind = "";
unsigned long lastRadarTime = 0;
unsigned long peopleNewUntil = 0;
unsigned long radarHoldUntil = 0;   // no scans until then (just woken)

// One-shot events, consumed by main.cpp
static volatile bool evBang = false, evClap = false, evDouble = false, evStep = false;
bool sensesTakeBang() { bool b = evBang; evBang = false; return b; }
bool sensesTakeClap() { bool b = evClap; evClap = false; return b; }
bool sensesTakeDoubleClap() { bool b = evDouble; evDouble = false; return b; }
static volatile unsigned long evKnockAt = 0;
unsigned long sensesTakeKnock() { unsigned long t = evKnockAt; evKnockAt = 0; return t; }
bool sensesTakeStep() { bool b = evStep; evStep = false; return b; }

// ===========================================================================
// EARS
// ===========================================================================
static const int    MIC_RATE   = 16000;
static const size_t MIC_LEN    = 320;          // 20 ms per chunk
static int16_t      micBuf[2][MIC_LEN];
static int          micW = 0;
static bool         micPaused = false;
static bool         micStarted = false;

// v3.5 ears. Tested in simulation against: a rumbly room (+-4 dB wander,
// clipping mic), talking at 62-76 dB, claps of different strength, a fan, and
// songs at 85-140 bpm through laptop / phone / Bluetooth speakers at 60-70 dB.
//
//  - Claps: the PEAK sample jumps up suddenly out of quiet. (Average loudness
//    can't see them: the mic clips at ~90 dB, so a clap's 20 ms average only
//    reaches ~76 dB.)
//  - Talking: loud on average and constantly changing.
//  - Music: "something new starts" pulses (onsets) that repeat at a steady
//    tempo (autocorrelation over 6 s), with no speech-like pauses.
static const float VOICE_DB   = 10;    // short-term loudness this far above the room...
static const float VOICE_MOD  = 3;     // ...that keeps changing (syllables) = talking
static const float CLAP_PK    = 18;    // peak this far above the room's usual peaks...
static const float CLAP_JUMP  = 14;    // ...jumping up this much in one 20 ms step...
static const float CLAP_DROP  = 8;     // ...and fading this much within 120 ms = clap / knock
static const float BANG_DB      = 40; // a clipped, short hit this loud (avg over room) = bang
                                       // (Mohanad's real claps clip at 28-38 dB: those are claps)
static const float BANG_RING_DB = 30; // a clipped hit that rings > 120 ms (dropped book) = bang
static const float BEAT_START = 0.24f; // tempo strength needed to start dancing
static const float BEAT_KEEP  = 0.18f; // ...and to keep dancing
static const float PAUSES_MAX = 0.30f; // more speech-like pauses than this = not music...
static const float BEAT_SURE  = 0.35f; // ...unless the beat is this steady (talking stays < 0.2)
static const float PLAYING_DB = 6;     // music must be playing NOW: this far above the room

static const int   HIST = 300;                 // onset history: 6 s of 20 ms chunks
static float onsetHist[HIST];
static int   histN = 0;
static unsigned long lastTempoEval = 0;
static int   tempoLag = 0, tempoHits = 0;
float beatScore = 0;                           // for the System page

static unsigned long listenUntil = 0, danceUntil = 0;
static unsigned long earsDeafUntil = 0;     // ignore sound (button clicks, plug, mic start)
static int   warm = 0;                      // first second: just learn the room
static float hpX1 = 0, hpY1 = 0, hiLp = 0;
static float roomDb = 0, roomPk = 0;        // the room's usual level / usual peaks
static int   loudRun = 0;                   // chunks in a row well above the room
static float powAvg = 0, modAvg = 0, prevLvl = 0, prevPk = 0;
static float avgL = 0, avgH = 0;            // ~150 ms averages (for onsets)
static float peakHold = 0, pauses = 1;      // speech-like pauses detector
static int   voiceRun = 0;
static float quietAvg = 0;                  // how loud it was just before (dB over room)

// Transient candidate: something jumped up; decide in the next few chunks
static bool  candActive = false;
static float candMax = 0;
static unsigned long candTime = 0;
static bool  candClipped = false;
// A clap stands alone: it is held back briefly and dropped if another hit
// follows (drums at the start of a song, before the tempo is known).
// Two hits = a double clap (still a clap); three or more = rhythm.
static unsigned long clapPendingAt = 0, clapLastHit = 0, rhythmUntil = 0;
static volatile unsigned long imuBumpAt = 0;   // last time the stick itself was knocked (poke, landing, handling)
static int clapBurst = 0, clapLoud = 0;   // clapLoud: loud chunks while waiting (talking?)

float lastBangDb = 0;                       // for main.cpp: how loud the last bang was

void earsIgnore(uint32_t ms) { earsDeafUntil = max(earsDeafUntil, millis() + ms); }

void micPause() {
  micPaused = true;
  if (micStarted) { M5.Mic.end(); micStarted = false; }
}
void micResume() { micPaused = false; }

static void micStart() {
  if (micStarted || micPaused || !earsEnabled) return;
  if (M5.Speaker.isRunning()) return;            // they share the audio pins
  auto cfg = M5.Mic.config();
  cfg.sample_rate = MIC_RATE;
  cfg.noise_filter_level = 0;
  M5.Mic.config(cfg);
  micStarted = M5.Mic.begin();
  if (micStarted) earsIgnore(500);               // the codec "pops" when it starts
}

// Tempo: autocorrelation of the onset pulses over the last 6 s.
// Music repeats itself every beat (and every 2 beats); talking and room noise don't.
static void evalTempo(unsigned long now, bool musicOn, float overRoom) {
  static float x[HIST];
  float mean = 0;
  for (int i = 0; i < HIST; i++) { x[i] = onsetHist[(histN + i) % HIST]; mean += x[i]; }
  mean /= HIST;
  float e = 1e-9f;
  for (int i = 0; i < HIST; i++) { x[i] -= mean; e += x[i] * x[i]; }
  float best = 0; int bl = 0;
  for (int lag = 16; lag <= 75; lag++) {        // 320..1500 ms
    float c = 0;
    for (int i = lag; i < HIST; i++) c += x[i] * x[i - lag];
    c /= e;
    if (2 * lag < HIST - 50) {                  // a matching double period: typical of beats
      float c2 = 0;
      for (int i = 2 * lag; i < HIST; i++) c2 += x[i] * x[i - 2 * lag];
      c = 0.7f * c + 0.3f * c2 / e;
    }
    if (c > best) { best = c; bl = lag; }
  }
  beatScore = best;
  float need = musicOn ? BEAT_KEEP : BEAT_START;
  // Drum-heavy songs have gaps between hits that look like speech pauses, so a
  // very steady beat wins anyway. The 6 s history outlives the song, so the
  // sound must still be there (v3.5 danced for 5 s after the music stopped).
  bool musicLike = pauses < PAUSES_MAX || best >= BEAT_SURE;
  if (best >= need && musicLike && overRoom >= PLAYING_DB) {
    bool same = abs(bl - tempoLag) <= 2 || abs(bl - 2 * tempoLag) <= 2 || abs(2 * bl - tempoLag) <= 2;
    tempoHits = same ? tempoHits + 1 : 1;
    tempoLag = bl;
    if (tempoHits >= (musicOn ? 1 : 3)) {      // the same tempo 3 times in a row (1.5 s)
      uint32_t p = bl * 20;
      while (p > 750) p /= 2;                   // bob on the beat, not on the bar
      if (p < 400) p *= 2;                      // ...nor on every hi-hat
      beatPeriodMs = p;
      danceUntil = now + 2500;
    }
  } else {
    tempoHits = 0;
  }
}

static void processChunk(const int16_t* d, size_t n, unsigned long now) {
  // High-pass (~75 Hz) removes the mic's rumble/hum. Also a treble band
  // (> ~1.5 kHz) for hi-hats / snares, and the peak sample for claps.
  double sum = 0, hsum = 0; float pkAbs = 0;
  float x1 = hpX1, y1 = hpY1, hl = hiLp;
  for (size_t i = 0; i < n; i++) {
    float x = d[i];
    float y = x - x1 + 0.97f * y1; x1 = x; y1 = y;
    sum += (double)y * y;
    float ay = fabsf(y); if (ay > pkAbs) pkAbs = ay;
    hl += (y - hl) * 0.45f;
    float h = y - hl; hsum += (double)h * h;
  }
  hpX1 = x1; hpY1 = y1; hiLp = hl;
  float lvl = 20.0f * log10f(sqrtf(sum / n) + 1.0f);
  float pk  = 20.0f * log10f(pkAbs + 1.0f);
  float hi  = 10.0f * log10f(hsum / n + 1.0f);
  micLevelDb = lvl;

  bool deaf = now < earsDeafUntil || lvl < 15;   // clicks, plugging in, mic start, digital silence
  if (deaf) { candActive = false; goto done; }

  // First second after boot: learn the room
  if (warm < 50) {
    if (warm == 0) { roomDb = lvl; roomPk = pk; peakHold = lvl; }
    warm++;
    roomDb += (lvl - roomDb) * 0.1f; roomPk += (pk - roomPk) * 0.1f;
    avgL = lvl; avgH = hi; prevLvl = lvl; prevPk = pk; powAvg = powf(10, lvl / 10);
    micFloorDb = roomDb;
    goto done;
  }

  {
    float dd = lvl - roomDb;
    bool musicOn = now < danceUntil;

    // Room level follows the usual quiet level and ignores loud moments.
    // After 10 s of constant loudness (a fan, AC, music) it creeps up 0.3 dB/s.
    if (dd < 6) { roomDb += (lvl - roomDb) * 0.02f; roomPk += (pk - roomPk) * 0.02f; loudRun = 0; }
    else if (++loudRun > 500 && !musicOn) { roomDb += 0.006f; roomPk += 0.006f; }
    micFloorDb = roomDb;

    // Speech has pauses: the level keeps dropping 10 dB below its recent peaks
    peakHold = max(lvl, peakHold - 0.3f);
    pauses += ((lvl < peakHold - 10 ? 1.0f : 0.0f) - pauses) * 0.0067f;   // ~3 s average

    // ---- Someone talking: loud on average AND constantly changing ----
    powAvg += (powf(10, lvl / 10) - powAvg) * 0.08f;
    float vdd = 10.0f * log10f(powAvg + 1.0f) - roomDb;
    modAvg += (fabsf(lvl - prevLvl) - modAvg) * 0.08f;
    prevLvl = lvl;
    if (vdd >= VOICE_DB && modAvg >= VOICE_MOD) { if (++voiceRun >= 25) listenUntil = now + 700; }
    else voiceRun = max(0, voiceRun - 1);

    // ---- Clap / knock / bang: a sudden peak out of quiet that fades fast ----
    bool talking = now < listenUntil + 1000;
    bool spike = pk - prevPk >= 8 && pk - roomPk >= 8;           // any clear hit, even a soft hi-hat
    // Every sharp hit counts toward the current burst (hits < 700 ms apart),
    // even while a clap candidate is still ringing.
    if (spike && now - clapLastHit > 60) {
      clapBurst = now - clapLastHit < 700 ? clapBurst + 1 : 1;
      clapLastHit = now;
    }
    if (clapPendingAt && dd >= 10) clapLoud++;
    if (clapPendingAt && clapLoud > 10) {        // > 0.2 s of sound: someone is talking
      clapPendingAt = 0;
      Serial.printf("EAR-EV clap dropped (talking)\n");
    }
    if (clapPendingAt && clapBurst >= 3) {
      clapPendingAt = 0; rhythmUntil = now + 1500;
      Serial.printf("EAR-EV clap dropped (rhythm: %d hits)\n", clapBurst);
    }
    if (clapPendingAt && (long)(now - imuBumpAt) < 400) {   // the stick was touched: a poke/knock, not a clap
      clapPendingAt = 0; evKnockAt = clapLastHit;   // someone knocked on it: a poke
      rhythmUntil = max(rhythmUntil, now + 700);    // its after-tap (finger lifting) is not a clap either
      Serial.printf("EAR-EV clap dropped (bump)\n");
    }
    if (clapPendingAt && now - clapLastHit >= 600 && now - clapPendingAt >= 100) {
      if (clapBurst >= 2) evDouble = true; else evClap = true;   // double clap = calling it
      clapPendingAt = 0;
      Serial.printf("EAR-EV CLAP %s\n", clapBurst >= 2 ? "double" : "single");
    }
    if (!musicOn && !talking && !candActive && now >= rhythmUntil && pk - roomPk >= CLAP_PK && pk - prevPk >= CLAP_JUMP && quietAvg < 6) {
      candActive = true; candMax = lvl; candTime = now; candClipped = pkAbs >= 29000;
    } else if (candActive) {
      candMax = max(candMax, lvl);
      if (pkAbs >= 29000) candClipped = true;
      if (candMax - lvl >= CLAP_DROP) {
        float over = candMax - roomDb;
        bool bumped = (long)(now - imuBumpAt) < 400;
        if (candClipped && over >= BANG_DB && !bumped) { evBang = true; lastBangDb = over; }
        else if (!clapPendingAt) { clapPendingAt = now; clapLoud = 0; }
        Serial.printf("EAR-EV %s over=%.1f clipped=%d\n", evBang ? "BANG" : "clap?", over, candClipped ? 1 : 0);
        candActive = false;
      } else if (now - candTime > 120) {
        // Stayed loud: not a clap. A clipped one that rings like this is a bang
        // (a dropped book rings; claps die in a flash, however loud).
        float over = candMax - roomDb;
        if (candClipped && over >= BANG_RING_DB && (long)(now - imuBumpAt) >= 400) { evBang = true; lastBangDb = over; }
        Serial.printf("EAR-EV %s max=%.1f clipped=%d\n", evBang ? "BANG(ring)" : "reject(stayed loud)", over, candClipped ? 1 : 0);
        candActive = false;
      }
    }
    quietAvg += (dd - quietAvg) * 0.15f;
    prevPk = pk;

    // ---- Music: onset pulses -> tempo ----
    float onset = max(0.0f, lvl - avgL) + max(0.0f, hi - avgH);
    avgL += (lvl - avgL) * 0.15f; avgH += (hi - avgH) * 0.15f;
    onsetHist[histN % HIST] = onset; histN++;
    if (onset > 4 && now - lastBeatTime > beatPeriodMs * 7 / 10) lastBeatTime = now;   // dance in phase
    if (histN >= HIST && now - lastTempoEval >= 500) { lastTempoEval = now; evalTempo(now, musicOn, vdd); }

    // Ear log on USB serial, twice a second (max over the window), for tuning
    static float mxL = 0, mxP = 0, mxV = 0, mxM = 0; static unsigned long lastLog = 0;
    mxL = max(mxL, lvl); mxP = max(mxP, pk - roomPk); mxV = max(mxV, vdd); mxM = max(mxM, modAvg);
    if (now - lastLog >= 500) {
      lastLog = now;
      Serial.printf("EAR lvl=%.1f max=%.1f room=%.1f pk+%.1f voice+%.1f mod=%.1f vrun=%d pauses=%.2f beat=%.2f lag=%d hits=%d %s\n",
                    lvl, mxL, roomDb, mxP, mxV, mxM, voiceRun, pauses, beatScore, tempoLag * 20, tempoHits,
                    now < danceUntil ? "DANCE" : now < listenUntil ? "LISTEN" : "-");
      mxL = mxP = mxV = mxM = 0;
    }
  }

done:
  dancing   = now < danceUntil;
  listening = !dancing && now < listenUntil;
}

static void earsUpdate(unsigned long now) {
  if (!earsEnabled || micPaused) { listening = dancing = false; if (micStarted && !earsEnabled) { M5.Mic.end(); micStarted = false; } return; }
  if (!micStarted) { micStart(); return; }
  if (M5.Mic.isRecording() <= 1) {
    if (M5.Mic.record(micBuf[micW], MIC_LEN, MIC_RATE)) {
      micW ^= 1;
      processChunk(micBuf[micW], MIC_LEN, now);
    }
  }
}

// ===========================================================================
// STEPS + ROCKING (fed by every IMU sample from main.cpp, ~66 Hz)
// ===========================================================================
static float lpMag = 1.0f;
static bool  stepAbove = false;
static unsigned long lastCand = 0;
static int   stepStreak = 0;
static bool  stepConfirmed = false;

// Rocking: oscillation along the busiest axis, gravity removed
static float rlp[3] = {0, 0, 1};
static float rvar[3] = {0, 0, 0};
static int   rSign = 0;
static float rPeak = 0;
static unsigned long rLastCross = 0;
static uint32_t rHalf[4] = {0};
static int   rValid = 0;
static float recentMagPeak = 1.0f;

void sensesImu(float ax, float ay, float az, float mag, unsigned long now) {
  if (fabsf(mag - 1.0f) > 0.12f) imuBumpAt = now;   // even a faint wobble: IMU often misses a tap's peak
  // ---- Step detection: peaks in the smoothed magnitude ----
  lpMag += (mag - lpMag) * 0.3f;
  if (!stepAbove && lpMag > 1.13f) stepAbove = true;
  else if (stepAbove && lpMag < 1.0f) {
    stepAbove = false;
    uint32_t iv = now - lastCand;
    lastCand = now;
    if (iv >= 300 && iv <= 2000) {
      stepStreak++;
      if (stepStreak >= 5) {
        int add = stepConfirmed ? 1 : 5;      // count the warm-up steps too
        stepConfirmed = true;
        stepsToday += add; stepsTotal += add;
        lastStepTime = now;
        evStep = true;
      }
    } else {
      stepStreak = 1;
      stepConfirmed = false;
    }
  }
  if (now - lastCand > 2000) { stepConfirmed = false; }
  walking = stepConfirmed && now - lastStepTime < 2500;

  // ---- Rocking ----
  float a[3] = { ax, ay, az };
  float dyn[3];
  for (int i = 0; i < 3; i++) {
    rlp[i] += (a[i] - rlp[i]) * 0.03f;
    dyn[i] = a[i] - rlp[i];
    rvar[i] += (dyn[i] * dyn[i] - rvar[i]) * 0.02f;
  }
  int k = rvar[0] > rvar[1] ? (rvar[0] > rvar[2] ? 0 : 2) : (rvar[1] > rvar[2] ? 1 : 2);
  float v = dyn[k];
  rPeak = max(rPeak, fabsf(v));
  recentMagPeak = max(recentMagPeak * 0.995f, mag);

  int s = v > 0.04f ? 1 : v < -0.04f ? -1 : 0;
  if (s != 0 && s != rSign) {
    if (rSign != 0) {
      uint32_t half = now - rLastCross;
      bool ok = half >= 250 && half <= 1300 && rPeak >= 0.05f && rPeak <= 0.6f;
      if (ok) { rHalf[rValid % 4] = half; rValid++; } else rValid = 0;
    }
    rSign = s; rLastCross = now; rPeak = 0;
  }
  bool steady = false;
  if (rValid >= 4) {
    uint32_t mn = 99999, mx = 0;
    for (int i = 0; i < 4; i++) { mn = min(mn, rHalf[i]); mx = max(mx, rHalf[i]); }
    steady = mx < mn * 1.8f;
  }
  rocking = steady && now - rLastCross < 1500 && !walking && recentMagPeak < 1.35f;
  if (now - rLastCross > 1500) rValid = 0;
}

// ===========================================================================
// PEOPLE RADAR (BLE): a short passive listen every few minutes
// ===========================================================================
static const uint32_t RADAR_EVERY_MS = 180000;
static const uint32_t RADAR_SCAN_S   = 2;
static bool bleReady = false;
static volatile bool radarDone = false;
static bool radarBusy = false;
static unsigned long lastRadarStart = 0;

static void radarComplete(NimBLEScanResults) { radarDone = true; }

static const char* companyName(uint16_t id) {
  switch (id) {
    case 0x004C: return "Apple";
    case 0x0075: return "Samsung";
    case 0x00E0: return "Google";
    case 0x0006: return "Microsoft";
    case 0x038F: return "Xiaomi";
    case 0x027D: return "Huawei";
    case 0x0157: return "Huami";
    default:     return nullptr;
  }
}

static void radarUpdate(unsigned long now) {
  if (radarBusy && radarDone) {
    radarDone = false; radarBusy = false;
    NimBLEScan* scan = NimBLEDevice::getScan();
    NimBLEScanResults res = scan->getResults();
    int count = 0;
    int kinds[8] = {0};
    static const uint16_t ids[7] = { 0x004C, 0x0075, 0x00E0, 0x0006, 0x038F, 0x027D, 0x0157 };
    for (int i = 0; i < res.getCount(); i++) {
      NimBLEAdvertisedDevice d = res.getDevice(i);
      if (d.getRSSI() < -75) continue;           // only things close by
      count++;
      if (d.haveManufacturerData()) {
        std::string m = d.getManufacturerData();
        if (m.size() >= 2) {
          uint16_t id = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
          for (int j = 0; j < 7; j++) if (ids[j] == id) kinds[j]++;
        }
      }
    }
    scan->clearResults();
    NimBLEDevice::deinit(true);      // Bluetooth fully off until the next look (saves battery)
    bleReady = false;
    int best = -1, bestN = 0;
    for (int j = 0; j < 7; j++) if (kinds[j] > bestN) { bestN = kinds[j]; best = j; }
    peopleTopKind = best >= 0 ? companyName(ids[best]) : "";
    if (peopleNearby >= 0 && count >= peopleNearby + 2 && count >= 2) peopleNewUntil = now + 6000;
    peopleNearby = count;
    lastRadarTime = now;
    if (count > 0) pet.happiness = min(100, pet.happiness + 1);   // company is nice
    return;
  }
  if (radarBusy) return;

  bool allowed = radarEnabled && restPhase == REST_NONE && !runningHot && !showsRunning() &&
                 (batteryShown < 0 || batteryShown > 15 || powerState != PWR_BATTERY) &&
                 !wifiScanInProgress && currentScreen != SCREEN_BOOT && currentScreen != SCREEN_HATCH;
  if (!allowed || now < radarHoldUntil) return;
  if (lastRadarStart != 0 && now - lastRadarStart < RADAR_EVERY_MS) return;
  if (lastRadarStart == 0 && now < 20000) return;          // first look ~20 s after boot

  if (!bleReady) {
    NimBLEDevice::init("Sila");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);       // passive: just listen, never ask devices for anything
    scan->setInterval(97);
    scan->setWindow(37);
    scan->setDuplicateFilter(true);
    bleReady = true;
  }
  lastRadarStart = now;
  radarDone = false;
  radarBusy = NimBLEDevice::getScan()->start(RADAR_SCAN_S, radarComplete, false);
}

// ===========================================================================
// Update (every loop)
// ===========================================================================
void sensesUpdate() {
  unsigned long now = millis();
  static unsigned long lastT = 0;
  float dt = lastT ? (now - lastT) / 1000.0f : 0; lastT = now;

  // Just woke up (any way): give it 30 s before a Bluetooth scan
  static bool wasResting = false;
  bool resting = restPhase != REST_NONE;
  if (wasResting && !resting) radarHoldUntil = max(radarHoldUntil, now + 30000);
  wasResting = resting;

  earsUpdate(now);
  radarUpdate(now);

  // New pet-day: roll the step counter
  if (pet.ageDays != stepsDayIdx) {
    stepsBest = max(stepsBest, stepsToday);
    stepsToday = 0;
    stepsDayIdx = pet.ageDays;
  }

  // Rocking makes it drowsy (only when it is awake on the home screen)
  bool canDoze = currentScreen == SCREEN_HOME && restPhase == REST_NONE && currentActivity == ACT_NONE;
  if (rocking && canDoze) rockDrowsy = min(1.0f, rockDrowsy + dt / 6.0f);
  else                    rockDrowsy = max(0.0f, rockDrowsy - dt / 4.0f);
}
