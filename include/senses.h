#pragma once
// Sila - senses (senses.cpp): ears, steps, rocking, people radar.
#include <Arduino.h>

extern bool earsEnabled, radarEnabled;

extern float micLevelDb, micFloorDb;      // loudness now / adaptive quiet level (dB)
extern bool  listening, dancing;          // someone talking / music with a beat
extern uint32_t beatPeriodMs;
extern float beatScore;                   // how strongly it hears a steady beat (0..1)
extern unsigned long lastBeatTime;

extern bool  walking;
extern unsigned long lastStepTime;
extern uint32_t stepsToday, stepsTotal, stepsBest;
extern uint16_t stepsDayIdx;

extern float rockDrowsy;                  // 0..1, reaches 1 -> falls asleep
extern bool  rocking;

extern int   peopleNearby;                // nearby Bluetooth devices, -1 = not scanned yet
extern const char* peopleTopKind;
extern unsigned long lastRadarTime, peopleNewUntil, radarHoldUntil;
extern char     lastResetWhy[16];      // why it last restarted unexpectedly ("" = never)
extern uint32_t lastResetCount;

void sensesUpdate();
void sensesImu(float ax, float ay, float az, float mag, unsigned long now);
void micPause();
void micResume();
void earsIgnore(uint32_t ms);   // deaf for a moment (button clicks, plugging in)
extern float lastBangDb;
bool sensesTakeBang();
bool sensesTakeClap();
bool sensesTakeDoubleClap();   // clap-clap: you are calling it
unsigned long sensesTakeKnock(); // millis of a tap the mic heard while the stick moved (0 = none)
bool sensesTakeStep();
