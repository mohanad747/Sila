// Sila - "Catch", a tilt mini-game.
//
// Tilt the stick left/right to move Sila along the bottom. Catch the falling
// WiFi snacks (+1) and hearts (+3), dodge the bugs. A missed snack or a caught
// bug costs one of 3 lives. It speeds up as the score grows.
// At the end the pet gets happier (and a little fed) depending on the score.
//
// A = start / pause / play again, hold B = back to the menu.

#include <Arduino.h>
#include <M5Unified.h>
#include "ui.h"
#include "theme.h"

using namespace theme;

namespace {

enum GState { G_READY, G_PLAY, G_PAUSE, G_OVER };
enum Kind : uint8_t { K_SNACK, K_HEART, K_BUG };

struct Drop { float x, y, vy; Kind kind; bool alive; };

const int   TOP      = 16;              // play area starts under the score line
const int   PLAYER_Y = SCR_H - 16;      // centre of Sila's little face
const int   PLAYER_W = 34, PLAYER_H = 20;
const int   MAX_DROPS = 8;
const float MAX_SPEED = 260;            // px/s at full tilt
const float TILT_FULL = 0.45f;          // tilt (in g) for full speed (~27 degrees)
const float TILT_DEAD = 0.05f;

GState state = G_READY;
Drop   drops[MAX_DROPS];
float  px = SCR_W / 2;
int    score = 0, lives = 3;
unsigned long lastT = 0, nextSpawn = 0, hurtUntil = 0, catchUntil = 0, overAt = 0;
int    rewardHappy = 0, rewardFood = 0;
bool   newBest = false;

float fallSpeed()  { return min(150.0f, 48.0f + score * 2.2f); }
uint32_t spawnGap(){ return (uint32_t)max(420, 1150 - score * 18); }

void spawn(unsigned long now) {
  for (auto& d : drops) {
    if (d.alive) continue;
    int r = random(0, 100);
    d.kind  = r < 8 ? K_HEART : r < 8 + min(30, 12 + score) ? K_BUG : K_SNACK;   // more bugs later
    d.x     = random(12, SCR_W - 12);
    d.y     = TOP;
    d.vy    = fallSpeed() * (d.kind == K_HEART ? 1.15f : 1.0f) * random(90, 111) / 100.0f;
    d.alive = true;
    break;
  }
  nextSpawn = now + spawnGap();
}

void start(unsigned long now) {
  for (auto& d : drops) d.alive = false;
  score = 0; lives = 3; px = SCR_W / 2;
  hurtUntil = catchUntil = 0;
  state = G_PLAY; lastT = now; nextSpawn = now + 600;
  sndGame(4);
}

void finish(unsigned long now) {
  state = G_OVER; overAt = now;
  // Rewards: playing together makes it happy; the snacks it caught feed it a bit
  rewardHappy = min(15, 2 + score / 2);
  rewardFood  = min(10, score / 3);
  pet.happiness = min(100, pet.happiness + rewardHappy);
  pet.hunger    = min(100, pet.hunger + rewardFood);
  newBest = score > gameBest;
  if (newBest) gameBest = score;
  saveState();
  sndGame(3);
}

// ---- drawing ----
void drawSnack(int x, int y, uint16_t c) {           // a WiFi symbol
  for (int i = 0; i < 3; i++) fb.fillArc(x, y + 5, 2 + i * 4, 3 + i * 4, 225, 315, c);
  fb.fillCircle(x, y + 5, 2, c);
}
void drawHeartAt(int x, int y, int r, uint16_t c) {
  fb.fillCircle(x - r, y, r, c);
  fb.fillCircle(x + r, y, r, c);
  fb.fillTriangle(x - 2 * r, y + 1, x + 2 * r, y + 1, x, y + 2 * r + 2, c);
}
void drawBug(int x, int y, unsigned long now) {        // a little red virus with wiggly legs
  int w = (now / 150) % 2;
  for (int i = -1; i <= 1; i++) {
    fb.drawLine(x - 5, y + i * 3, x - 9, y + i * 3 + (w ? 1 : -1), ALERT);
    fb.drawLine(x + 5, y + i * 3, x + 9, y + i * 3 + (w ? -1 : 1), ALERT);
  }
  fb.fillEllipse(x, y, 6, 5, ALERT);
  fb.fillCircle(x - 2, y - 1, 1, BG);
  fb.fillCircle(x + 2, y - 1, 1, BG);
}
void drawPlayer(unsigned long now) {                   // Sila's face, small
  int x = (int)px, y = PLAYER_Y;
  bool hurt = now < hurtUntil, happy = now < catchUntil;
  uint16_t body = hurt ? dim(ALERT, 0.5f) : rgb(22, 52, 66);
  fb.fillRoundRect(x - PLAYER_W / 2, y - PLAYER_H / 2, PLAYER_W, PLAYER_H, 8, body);
  fb.drawRoundRect(x - PLAYER_W / 2, y - PLAYER_H / 2, PLAYER_W, PLAYER_H, 8, hurt ? ALERT : TEAL);
  if (hurt) {                                          // >_<
    fb.drawLine(x - 10, y - 5, x - 5, y - 2, EYE); fb.drawLine(x - 10, y + 1, x - 5, y - 2, EYE);
    fb.drawLine(x + 10, y - 5, x + 5, y - 2, EYE); fb.drawLine(x + 10, y + 1, x + 5, y - 2, EYE);
  } else if (happy) {                                  // ^ ^
    fb.fillArc(x - 7, y, 3, 5, 200, 340, EYE);
    fb.fillArc(x + 7, y, 3, 5, 200, 340, EYE);
  } else {
    fb.fillRoundRect(x - 10, y - 6, 6, 8, 2, EYE);
    fb.fillRoundRect(x + 4, y - 6, 6, 8, 2, EYE);
  }
  fb.fillArc(x, y + 3, 3, 4, 20, 160, happy ? TERRA : GREY);   // little mouth
}
void textMid(int y, const char* s, uint16_t c, int size = 1) {
  fb.setTextSize(size);
  fb.setTextColor(c);
  fb.setCursor(SCR_W / 2 - fb.textWidth(s) / 2, y);
  fb.print(s);
  fb.setTextSize(1);
}

}  // namespace

void gameEnter() {
  state = G_READY;
  for (auto& d : drops) d.alive = false;
  px = SCR_W / 2;
}

void gameButtonA() {
  unsigned long now = millis();
  switch (state) {
    case G_READY: start(now); break;
    case G_PLAY:  state = G_PAUSE; break;
    case G_PAUSE: state = G_PLAY; lastT = now; break;
    case G_OVER:  if (now - overAt > 800) start(now); break;   // don't restart by accident
  }
}

void gameDraw() {
  unsigned long now = millis();
  float dt = lastT ? constrain((now - lastT) / 1000.0f, 0.0f, 0.06f) : 0;
  lastT = now;

  // ---- update ----
  float tilt = tiltRight();
  float t = fabsf(tilt) < TILT_DEAD ? 0 : constrain(tilt / TILT_FULL, -1.0f, 1.0f);
  if (state == G_READY || state == G_PLAY) {
    px += t * MAX_SPEED * dt;
    px = constrain(px, PLAYER_W / 2.0f, SCR_W - PLAYER_W / 2.0f);
  }
  if (state == G_PLAY) {
    if (now >= nextSpawn) spawn(now);
    for (auto& d : drops) {
      if (!d.alive) continue;
      d.y += d.vy * dt;
      bool inX = fabsf(d.x - px) < PLAYER_W / 2 + 5;
      bool inY = d.y > PLAYER_Y - PLAYER_H / 2 - 4 && d.y < PLAYER_Y + 4;
      if (inX && inY) {                                // caught it
        d.alive = false;
        if (d.kind == K_BUG) { lives--; hurtUntil = now + 500; sndGame(2); }
        else { score += d.kind == K_HEART ? 3 : 1; catchUntil = now + 300; sndGame(d.kind == K_HEART ? 1 : 0); }
      } else if (d.y > SCR_H + 6) {                    // fell past
        d.alive = false;
        if (d.kind == K_SNACK) { lives--; hurtUntil = now + 500; sndGame(2); }
      }
    }
    if (lives <= 0) finish(now);
  }

  // ---- draw ----
  fb.fillSprite(BG);
  char buf[32];
  fb.setTextSize(1);
  fb.setTextColor(SEC_PLAY); fb.setCursor(4, 4); fb.print("CATCH");
  snprintf(buf, sizeof(buf), "%d", score);
  textMid(4, buf, TEXT);
  for (int i = 0; i < 3; i++) drawHeartAt(SCR_W - 32 + i * 11, 6, 2, i < lives ? HEART : SEG_OFF);
  fb.drawFastHLine(0, TOP - 2, SCR_W, SEG_OFF);

  for (auto& d : drops) {
    if (!d.alive) continue;
    int x = (int)d.x, y = (int)d.y;
    if (d.kind == K_SNACK)      drawSnack(x, y - 4, SEC_WIFI);
    else if (d.kind == K_HEART) drawHeartAt(x, y - 2, 3, HEART);
    else                        drawBug(x, y, now);
  }
  drawPlayer(now);

  if (state == G_READY) {
    textMid(30, "CATCH", SEC_PLAY, 2);
    textMid(52, "tilt me left & right", TEXT);
    snprintf(buf, sizeof(buf), "best %d", gameBest);
    textMid(66, buf, GREY);
    textMid(84, "A start   hold B back", TEAL);
  } else if (state == G_PAUSE) {
    textMid(46, "paused", TEXT, 2);
    textMid(70, "A play   hold B quit", TEAL);
  } else if (state == G_OVER) {
    textMid(24, newBest ? "NEW BEST!" : "game over", newBest ? C_HUNGER : TEXT, 2);
    snprintf(buf, sizeof(buf), "score %d   best %d", score, gameBest);
    textMid(46, buf, TEXT);
    snprintf(buf, sizeof(buf), "+%d happy  +%d food", rewardHappy, rewardFood);
    textMid(60, buf, TERRA);
    textMid(80, "A again   hold B back", TEAL);
  }
  fb.pushSprite(0, 0);
}
