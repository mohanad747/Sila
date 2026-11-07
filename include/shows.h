#pragma once
// Sila - idle shows: little performances while it sits on your desk.
// shows.cpp decides when one runs and describes the face through a ShowPose;
// face.cpp applies the pose and calls back to draw props (ball, cube, fly...).
#include <Arduino.h>
#include <M5Unified.h>

enum MouthStyle { MOUTH_DASH, MOUTH_SMILE, MOUTH_BIG_SMILE, MOUTH_FROWN, MOUTH_O, MOUTH_YAWN,
                  MOUTH_CHEW, MOUTH_WAVY, MOUTH_NONE };

enum ShowId : uint8_t { SHOW_NONE = 0, SHOW_PEEK, SHOW_BALL, SHOW_CUBE, SHOW_EYE, SHOW_FLY, SHOW_SNACK, SHOW_COUNT };
enum IdleShows : uint8_t { SHOWS_LIVELY = 0, SHOWS_CALM = 1, SHOWS_OFF = 2 };

// What a show wants the face to do this frame. Offsets are canvas pixels.
struct ShowPose {
  bool  on = false;            // a show owns the face
  bool  hideFace = false;      // no eyes, brows or mouth (the cube replaces them)
  float ox = 0, oy = 0;        // whole face
  float lx = 0, ly = 0, rx = 0, ry = 0;      // one eye only (its brow stays put)
  float lsx = 1, lsy = 1, rsx = 1, rsy = 1;  // eye size
  float gx = 0, gy = 0;        // gaze
  float happy = 0, browRaise = 0, browSlant = 0, flat = 0;
  MouthStyle mouth = MOUTH_DASH;
  bool  blink = true;
  bool  blush = false;
  const char* badge = nullptr;
};

// The face's current layout, handed over every frame.
struct ShowStage {
  int   W, H, CX, CY, mouthY;
  float S, eyeOff, eyeW, eyeH;
  MouthStyle restMouth;
  bool  portrait;
};

extern uint8_t idleShows;      // IdleShows setting (saved)
extern float   faceTiltDelta;  // degrees between real gravity and the angle the face is held at (ui.cpp)

void  showsTick(unsigned long now, const ShowStage& stage);  // once per face frame, before the expression
void  showsDraw(M5Canvas* c, bool front);                    // props behind / in front of the face
const ShowPose& showsPose();
bool  showsRunning();                                        // a show is on stage (holds WiFi hunts and the radar)
bool  showsStart(ShowId id);                                 // start now if it is free (serial test command)
