#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include "senses.h"

// ============ Screen geometry (StickS3, landscape) ============
static const int SCR_W    = 240;
static const int SCR_H    = 135;
static const int HEADER_H = 14;


// ============ Enums & structs shared between UI and main ============

enum Screen {
  SCREEN_BOOT,
  SCREEN_HATCH,
  SCREEN_HOME,
  SCREEN_MENU,
  SCREEN_PET_STATUS,
  SCREEN_ENVIRONMENT,
  SCREEN_SYSINFO,
  SCREEN_CONTROLS,
  SCREEN_SETTINGS,
  SCREEN_DIAGNOSTICS,
  SCREEN_GAMEOVER,
  SCREEN_VITALS,
  SCREEN_WALKS,
  SCREEN_GAME
};

enum Activity { ACT_NONE, ACT_HUNT, ACT_DISCOVER, ACT_REST };

enum Stage { STAGE_BABY = 0, STAGE_TEEN = 1, STAGE_ADULT = 2, STAGE_ELDER = 3 };

enum Mood {
  MOOD_HUNGRY,
  MOOD_HAPPY,
  MOOD_CURIOUS,
  MOOD_BORED,
  MOOD_SICK,
  MOOD_EXCITED,
  MOOD_CALM
};

enum RestPhase { REST_NONE, REST_ENTER, REST_DEEP, REST_WAKE };

struct Pet {
  int hunger;
  int happiness;
  int health;
  unsigned long ageMinutes;
  unsigned long ageHours;
  unsigned long ageDays;
};

struct WifiStats {
  int netCount    = 0;
  int strongCount = 0;
  int hiddenCount = 0;
  int avgRSSI     = -100;
  int openCount   = 0;
  int wpaCount    = 0;
};

// Menu sizes (main logic and UI must agree)
static const int MAIN_MENU_COUNT = 9;
static const int CONTROLS_COUNT  = 6;
static const int SETTINGS_COUNT  = 9;

// ============ Framebuffer (defined in main.cpp) ============
extern M5Canvas fb;

// ============ Shared game state (defined in main.cpp) ============
extern Screen    currentScreen;
extern Activity  currentActivity;
extern RestPhase restPhase;

extern Pet       pet;
extern WifiStats wifiStats;

extern Mood      currentMood;
extern Stage     petStage;

extern bool      hungerEffectActive;
extern int       hungerEffectFrame;
extern bool      hasHatchedOnce;
extern bool      hatchTriggered;

extern bool      wifiScanInProgress;
extern unsigned long lastWifiScanTime;

enum SoundMode : uint8_t { SOUND_OFF = 0, SOUND_KEY = 1, SOUND_ALL = 2 };
extern uint8_t   soundMode;
extern uint8_t   tftBrightnessIndex;
extern uint8_t   volumeIndex;
extern bool      screenFlipped;
enum IdleMode : uint8_t { IDLE_DIM = 0, IDLE_BLACK = 1, IDLE_ON = 2 };
extern uint8_t   idleMode;        // what the screen does when you stop touching it
extern bool      lieDownSleep;    // lay it flat -> pet goes to bed
extern bool      petBedtime;      // currently asleep because it was laid down
extern bool      scanSaver;       // fewer WiFi scans
extern bool      autoRotate;      // home face follows gravity
extern float     faceRoll;        // -1..1 sideways lean for the face
extern float     gravAngle;       // degrees: rotation that keeps the face level with the world
extern bool      gravValid;       // false when lying flat (keep the last angle)
extern float     motionX, motionY;// screen-space movement (g, gravity removed)
extern uint8_t   faceTargetRot;   // rotation the face should be upright in
extern uint8_t   baseRot;         // rotation the panel is set to
extern unsigned long dizzyUntil;  // spiral eyes until then
extern bool      speakerAwake;    // amp powered (System page)
enum PowerState : uint8_t { PWR_BATTERY = 0, PWR_CHARGING = 1, PWR_FULL = 2 };
extern uint8_t   powerState;      // on battery / charging / plugged in and full
extern int       batteryShown;    // smoothed battery %
extern int       batteryMv;       // averaged battery voltage (mV)
extern float     chipTempC;       // chip temperature
extern bool      runningHot;      // throttling because it is hot
extern bool      rockedSleep;     // asleep because it was rocked
extern unsigned long startleUntil, stirUntil, noticeUntil, stepGoalUntil;
// v3.7 reactions (set in main.cpp, drawn by face.cpp)
extern unsigned long calledUntil;   // double clap: "you called?"
extern unsigned long giggleUntil;   // poked: giggles
extern unsigned long grumpyUntil;   // poked too much
extern uint8_t       pokeLevel;     // 1 giggle, 2 "hey!", 3 "stop poking!"
extern unsigned long fallUntil;     // falling: screams
extern unsigned long wheeUntil;     // thrown up and caught softly
extern unsigned long ouchUntil;     // landed hard
extern unsigned long zapUntil;      // just plugged in: eats electricity
extern uint16_t  autoSaveMs;

extern uint8_t   traitCuriosity;
extern uint8_t   traitActivity;
extern uint8_t   traitStress;

extern int       restFrameIndex;

extern int       mainMenuIndex;
extern int       controlsIndex;
extern int       settingsMenuIndex;

// "Play" interaction (shake the stick) - UI shows hearts while active
extern unsigned long playEffectUntil;

// ============ Functions the UI calls back into ============
void sndHatch();
void sndGame(int kind);           // 0 catch, 1 heart, 2 hurt, 3 game over, 4 start
float tiltRight();                // -1..1: how far the right side of the screen tips down
extern uint16_t gameBest;         // best Catch score (saved)
void saveState();

// Catch mini-game (game.cpp)
void gameEnter();
void gameButtonA();
void gameDraw();

// ============ UI API ============
// Robot-eyes face (face.cpp)
extern M5Canvas faceLand, facePort;  // made once in uiInit, never re-created
extern M5Canvas* faceCanvas;     // the one in use: the face is drawn here, then rotated onto fb
void faceReset();
void faceRender();
void faceRenderDead();
void faceStartIntro(bool blackFirst);   // boot / after hatching: eyes pop in
bool faceIntroDone();
float faceIntroScale();                 // 0..~1.1 size of the face during the intro
extern float faceMotionX, faceMotionY;  // movement in face coordinates (set by the UI)

void uiInit();
void uiOnScreenChange(Screen newScreen);
void uiDrawScreen(Screen screen, int mainMenuIndex, int controlsIndex, int settingsMenuIndex);
