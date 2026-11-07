// Sila - idle shows.
//
// While it sits on the desk with nothing going on, every few minutes it performs
// something short: peek-a-boo, plays with a ball, turns into a cube, loses an eye,
// chases a fly, or eats the WiFi it found. Picked at random, never the same one
// soon after itself, and mood changes the odds. Anything you do (clap, poke, talk,
// pick it up) stops the show at once: your reaction comes first, then "oops..".
//
// Shows are silent (you are working) and never change the pet's stats.
// Everything is drawn with shapes in canvas pixels, so it works in portrait too.

#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>
#include "ui.h"
#include "theme.h"
#include "shows.h"

using namespace theme;

uint8_t idleShows     = SHOWS_LIVELY;
float   faceTiltDelta = 0;

namespace {

ShowPose      pose;
ShowStage     st;
ShowId        current    = SHOW_NONE;
unsigned long startMs    = 0, lastTick = 0, nextDue = 0, oopsUntil = 0;
bool          oopsPending = false;
ShowId        recent[3]  = { SHOW_NONE, SHOW_NONE, SHOW_NONE };
float         T = 0, dt = 0.03f;           // seconds into the show, seconds this frame

const uint32_t LIVELY_MIN = 120000, LIVELY_MAX = 300000;
const uint32_t CALM_MIN   = 300000, CALM_MAX   = 600000;
const uint32_t FIRST_SHOW = 60000;         // first one this long after boot
const uint32_t BUSY_RETRY = 5000;
const float    TURNED_DEG = 55;            // turned further than this: the show stops, the face floats again

float clamp01(float v)                { return v < 0 ? 0 : v > 1 ? 1 : v; }
float seg(float t, float a, float b)  { return clamp01((t - a) / (b - a)); }
float smooth(float p)                 { return p * p * (3 - 2 * p); }
float outBack(float p) {
  const float c1 = 1.9f, c3 = c1 + 1;
  float q = p - 1;
  return 1 + c3 * q * q * q + c1 * q * q;
}
float rnd(float a, float b)           { return a + (b - a) * random(0, 1001) / 1000.0f; }
float lim(float v, float m)           { return v < -m ? -m : v > m ? m : v; }

// Real gravity in the canvas (x right, y down), and pushes from moving the stick.
void gravity(float g, float& ax, float& ay) {
  float d = faceTiltDelta * DEG_TO_RAD;
  ax = -sinf(d) * g + (gravValid ? 0.0f : tiltRight() * g);
  ay =  cosf(d) * g;
  ax -= faceMotionX * g * 3.0f;
  ay -= faceMotionY * g * 1.5f;
}

// ---------------------------------------------------------------------------
// Peek-a-boo: slides off, peeks in from the side, then from below, then BOO
// ---------------------------------------------------------------------------
bool runPeek() {
  pose.blink = false;
  float rightPeek  = st.W - st.eyeW * 0.2f - (st.CX - st.eyeOff);
  float bottomPeek = st.H - st.eyeH * 0.25f - st.CY;
  if (T < 0.5f) {                                      // glances where it is going
    pose.gx = -20; pose.browRaise = 3;
  } else if (T < 1.0f) {                               // slips away
    pose.ox = -st.W * smooth(seg(T, 0.5f, 1.0f));
    pose.gx = -20;
  } else if (T < 2.0f) {                               // gone...
    pose.ox = -st.W;
  } else if (T < 3.4f) {                               // ...right edge
    float in  = outBack(seg(T, 2.0f, 2.35f));
    float out = smooth(seg(T, 3.1f, 3.4f));
    pose.ox = st.W + (rightPeek - st.W) * in + (st.W - rightPeek) * out;
    pose.gx = -8; pose.mouth = MOUTH_NONE; pose.browRaise = 4;
    if (T > 2.6f && T < 2.72f) pose.lsy = pose.rsy = 0.12f;  // blinks at you
  } else if (T < 3.7f) {
    pose.ox = st.W;
  } else if (T < 5.0f) {                               // ...from below
    float in  = outBack(seg(T, 3.7f, 4.05f));
    float out = smooth(seg(T, 4.7f, 5.0f));
    pose.oy = st.H + (bottomPeek - st.H) * in + (st.H - bottomPeek) * out;
    pose.gy = -7; pose.mouth = MOUTH_NONE; pose.browRaise = 6;
  } else if (T < 5.3f) {
    pose.oy = st.H;
  } else if (T < 7.0f) {                               // BOO
    float p = outBack(seg(T, 5.3f, 5.75f));
    pose.lsx = pose.rsx = 0.2f + 1.0f * p - 0.2f * seg(T, 5.75f, 6.4f);
    pose.lsy = pose.rsy = pose.lsx;
    pose.mouth = MOUTH_BIG_SMILE; pose.browRaise = 6; pose.happy = 0.5f * seg(T, 5.9f, 6.4f);
    pose.badge = "boo!";
  } else {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Ball: bounces in, head-butts it around, then misses it
// ---------------------------------------------------------------------------
struct { float x, y, vx, vy, spin, faceX, hitT, missT, goneT; int hits, target; bool missed; } ball;

void initBall() {
  ball.x = -10; ball.y = st.H * 0.12f;
  ball.vx = rnd(70, 95); ball.vy = 0; ball.spin = 0;
  ball.faceX = 0; ball.hitT = -1; ball.hits = 0; ball.target = random(4, 7);
  ball.missed = false; ball.missT = 0; ball.goneT = -1;
  if (random(0, 2)) { ball.x = st.W + 10; ball.vx = -ball.vx; }
}

bool runBall() {
  const float r = max(6.0f, 9 * st.S), G = 300;
  float ax, ay;
  gravity(G, ax, ay);
  float floorY = st.H - 2 - r;
  float headHalf = st.eyeOff + st.eyeW / 2;
  float hitAge = ball.hitT < 0 ? 9 : T - ball.hitT;
  float bump = hitAge < 0.18f ? -9 * st.S * (1 - hitAge / 0.18f) : 0;
  const float sit = 16 * st.S;                         // sits lower, so the ball has room above it
  float headY = st.CY + sit - st.eyeH / 2 - 15 * st.S + bump;

  ball.vx += ax * dt; ball.vy += ay * dt;
  ball.x  += ball.vx * dt; ball.y += ball.vy * dt;
  ball.spin += ball.vx * dt / r;

  if (!ball.missed) {
    if (ball.vy > 0 && ball.y + r >= headY && ball.y < st.CY + sit && fabsf(ball.x - (st.CX + ball.faceX)) < headHalf) {
      ball.y = headY - r;
      ball.vy = -sqrtf(2 * G * max(12.0f, headY - r - 3)) * rnd(0.82f, 1.0f);   // just under the top edge
      float toMiddle = (st.CX - ball.x) * 0.5f;
      ball.vx = lim(toMiddle + rnd(-70, 70), 95);
      ball.hitT = T;
      if (++ball.hits >= ball.target) { ball.missed = true; ball.missT = T + 0.45f; }
    }
    if (ball.x < r)          { ball.x = r;          ball.vx =  fabsf(ball.vx) * 0.9f; }
    if (ball.x > st.W - r)   { ball.x = st.W - r;   ball.vx = -fabsf(ball.vx) * 0.9f; }
  }
  if (ball.y > floorY) {
    ball.y = floorY;
    ball.vy = -fabsf(ball.vy) * 0.62f;
    if (fabsf(ball.vy) < 25) ball.vy = 0;
    if (!ball.missed) { ball.missed = true; ball.missT = T; }   // dropped it for real
  }

  // Follows the ball with its head, gives up once it has missed
  bool aiming = !ball.missed && (ball.vy > -40 || ball.y > st.CY - st.H * 0.15f);
  float wantX = aiming ? lim(ball.x - st.CX, st.W - (st.CX + st.eyeOff + st.eyeW * 0.6f)) : 0;   // stays on screen
  ball.faceX += (wantX - ball.faceX) * min(1.0f, dt * 7);

  pose.ox = ball.faceX;
  pose.oy = sit * smooth(seg(T, 0, 0.5f)) * (ball.goneT < 0 ? 1 : 1 - smooth(seg(T, ball.goneT, ball.goneT + 0.4f))) + bump;
  pose.gx = lim((ball.x - (st.CX + ball.faceX)) * 0.16f, 20);
  pose.gy = lim((ball.y - st.CY - sit) * 0.11f, 7);
  pose.browRaise = 4;
  pose.mouth = MOUTH_SMILE;
  pose.blink = false;
  if (hitAge < 0.14f) { pose.lsy = pose.rsy = 0.72f; pose.lsx = pose.rsx = 1.12f; pose.happy = 0.5f; pose.mouth = MOUTH_BIG_SMILE; }

  if (ball.missed) {
    pose.mouth = MOUTH_O; pose.browRaise = 8;
    if (T > ball.missT + 0.3f) {                       // it rolls away
      float dir = ball.vx >= 0 ? 1.0f : -1.0f;
      if (ball.y >= floorY - 1) ball.vx += dir * 170 * dt;
    }
    bool gone = ball.x < -r - 4 || ball.x > st.W + r + 4;
    if (gone) {
      if (ball.goneT < 0) ball.goneT = T;
      pose.gx = pose.gy = 0;
      pose.happy = 0.45f; pose.mouth = MOUTH_SMILE; pose.blush = true;
      pose.badge = st.portrait ? "again?" : "one more?";
      if (T > ball.goneT + 1.6f) return false;
    }
  }
  return T < 24;                                       // never stuck on stage
}

void drawBall(M5Canvas* c) {
  const float r = max(6.0f, 9 * st.S);
  int x = (int)ball.x, y = (int)ball.y;
  float h = clamp01((st.H - 2 - r - ball.y) / st.H);
  int sw = (int)(r * (1.1f - h * 0.6f));
  if (x > -r - 4 && x < st.W + r + 4) {
    c->fillEllipse(x, st.H - 2, sw, 1, dim(TERRA, 0.45f - h * 0.3f));
    c->fillCircle(x, y, (int)r, TERRA);
    c->fillCircle(x + (int)(cosf(ball.spin) * r * 0.55f), y + (int)(sinf(ball.spin) * r * 0.55f), max(1, (int)(r * 0.22f)), dim(TERRA, 0.45f));
    c->fillCircle(x - (int)(r * 0.35f), y - (int)(r * 0.4f), max(1, (int)(r * 0.22f)), TEXT);
  }
}

// ---------------------------------------------------------------------------
// Cube: the eyes meet and pop into a 3D cube that spins, hops, then changes back
// ---------------------------------------------------------------------------
struct V3 { float x, y, z; };

struct { float yaw, pitch, a, cy, scale, look, blink; } cube;

V3 rot(float x, float y, float z) {
  float cy = cosf(cube.yaw), sy = sinf(cube.yaw), cp = cosf(cube.pitch), sp = sinf(cube.pitch);
  float x1 = x * cy + z * sy, z1 = -x * sy + z * cy;
  return { x1, y * cp - z1 * sp, y * sp + z1 * cp };
}

void proj(float x, float y, float z, int& sx, int& sy) {
  float a = cube.a * cube.scale;
  V3 p = rot(x * a, y * a, z * a);
  float d = cube.a * 5;
  float k = d / (d + p.z);
  sx = st.CX + (int)(p.x * k);
  sy = (int)cube.cy + (int)(p.y * k);
}

bool runCube() {
  const float A = 0.7f, B = 1.1f, SPIN_END = 5.0f, HOP_END = 6.2f, BACK = 7.6f, DONE = 8.3f, END = 8.9f;
  pose.blink = false;
  cube.a  = min(st.W, st.H) * 0.26f;
  cube.cy = st.H / 2.0f - 3;
  cube.yaw = cube.pitch = cube.look = 0;
  cube.blink = 1;
  cube.scale = 0;

  if (T < A) {                                         // eyes rush together
    float p = smooth(seg(T, 0, A));
    pose.lx =  st.eyeOff * p; pose.rx = -st.eyeOff * p;
    pose.lsx = pose.rsx = 1 - 0.45f * p;
    pose.lsy = pose.rsy = 1 - 0.1f * p;
    pose.mouth = MOUTH_O; pose.browRaise = 6;
  } else if (T < DONE - 0.45f) {
    pose.hideFace = true;
    cube.scale = T < B ? outBack(seg(T, A, B)) : 1.0f;
    if (T > BACK) cube.scale = 1 - smooth(seg(T, BACK, DONE - 0.45f));
    if (T < SPIN_END) {
      cube.yaw   = 4 * PI * smooth(seg(T, A, SPIN_END));
      cube.pitch = sinf(T * 2.3f) * 0.55f * (1 - seg(T, SPIN_END - 1, SPIN_END));
    } else {
      float t = T - SPIN_END;
      cube.look = sinf(t * 1.9f);
      cube.yaw  = cube.look * 0.3f;
      cube.pitch = 0.16f;
      if (T < HOP_END) cube.cy -= fabsf(sinf(PI * t / 0.6f)) * 13 * st.S;
      if (T > 6.6f && T < 6.75f) cube.blink = 0.1f;
      if (T > SPIN_END + 0.3f && T < BACK) pose.badge = st.portrait ? "cube!" : "I'm a cube!";
    }
  } else if (T < END) {                                // back to itself
    float p = outBack(seg(T, DONE - 0.45f, DONE));
    pose.lx =  st.eyeOff * (1 - p); pose.rx = -st.eyeOff * (1 - p);
    pose.lsx = pose.rsx = 0.55f + 0.45f * p;
    pose.happy = 0.4f; pose.mouth = MOUTH_SMILE;
  } else {
    return false;
  }
  return true;
}

void drawCube(M5Canvas* c) {
  if (cube.scale <= 0.02f) return;
  static const int8_t F[6][7] = {   // normal xyz, then which corners: (sx,sy) pattern by axis
    { 0, 0,-1, 0,0,0,0 }, { 0, 0, 1, 0,0,0,0 }, {-1, 0, 0, 0,0,0,0 },
    { 1, 0, 0, 0,0,0,0 }, { 0,-1, 0, 0,0,0,0 }, { 0, 1, 0, 0,0,0,0 } };
  const float Lx = -0.4f, Ly = -0.6f, Lz = -0.69f;
  uint16_t base[6] = { rgb(18, 44, 62), rgb(18, 44, 62), TEAL, TEAL, TERRA, TERRA };

  // Soft shadow under it
  int sw = (int)(cube.a * 1.1f * cube.scale * (cube.cy > st.H / 2.0f - 6 ? 1.0f : 0.8f));
  c->fillEllipse(st.CX, min(st.H - 3, (int)(st.H / 2.0f + cube.a * 1.62f)), sw, 2, rgb(30, 42, 50));

  for (int f = 0; f < 6; f++) {
    float nx = F[f][0], ny = F[f][1], nz = F[f][2];
    V3 n = rot(nx, ny, nz);
    if (n.z >= -0.01f) continue;                       // facing away
    float ux = nz != 0 ? 1 : 0, uy = nz != 0 ? 0 : (nx != 0 ? 1 : 0), uz = nz != 0 ? 0 : (nx != 0 ? 0 : 1);
    float vx = nz != 0 ? 0 : (nx != 0 ? 0 : 1), vy = nz != 0 ? 1 : 0, vz = nx != 0 ? 1 : 0;
    int px[4], py[4];
    const int8_t su[4] = { -1, 1, 1, -1 }, sv[4] = { -1, -1, 1, 1 };
    for (int i = 0; i < 4; i++)
      proj(nx + ux * su[i] + vx * sv[i], ny + uy * su[i] + vy * sv[i], nz + uz * su[i] + vz * sv[i], px[i], py[i]);
    float light = 0.32f + 0.68f * max(0.0f, n.x * Lx + n.y * Ly + n.z * Lz);
    uint16_t col = dim(base[f], light);
    c->fillTriangle(px[0], py[0], px[1], py[1], px[2], py[2], col);
    c->fillTriangle(px[0], py[0], px[2], py[2], px[3], py[3], col);
    for (int i = 0; i < 4; i++) {
      int j = (i + 1) % 4;
      c->drawLine(px[i], py[i], px[j], py[j], dim(EYE, 0.85f));
    }
    if (f == 0) {                                      // its eyes and smile live on the front
      const float gaze = cube.look * 0.08f;
      for (int side = -1; side <= 1; side += 2) {
        float ex = side * 0.42f + gaze, ey = -0.12f, hw = 0.25f, hh = 0.3f * cube.blink;
        int qx[4], qy[4];
        for (int i = 0; i < 4; i++) proj(ex + su[i] * hw, ey + sv[i] * hh, -1.0f, qx[i], qy[i]);
        c->fillTriangle(qx[0], qy[0], qx[1], qy[1], qx[2], qy[2], EYE);
        c->fillTriangle(qx[0], qy[0], qx[2], qy[2], qx[3], qy[3], EYE);
      }
      int m0x, m0y, m1x, m1y, m2x, m2y;
      proj(gaze - 0.16f, 0.5f, -1.0f, m0x, m0y);
      proj(gaze,         0.62f, -1.0f, m1x, m1y);
      proj(gaze + 0.16f, 0.5f, -1.0f, m2x, m2y);
      c->drawLine(m0x, m0y, m1x, m1y, EYE);
      c->drawLine(m1x, m1y, m2x, m2y, EYE);
    }
  }
}

// ---------------------------------------------------------------------------
// Lost eye: one eye falls off, the other stares, it hops back. Nobody saw.
// ---------------------------------------------------------------------------
struct { float x, y, vx, vy, restX, restY, squashT; bool down; } eye;

void initEye() {
  eye.x = eye.y = 0; eye.vx = rnd(45, 80); eye.vy = -150;
  eye.down = false; eye.squashT = -1; eye.restX = eye.restY = 0;
}

bool runEye() {
  const float FALL = 0.55f, STARE = 3.0f, HOPS = 3.7f, HOME = 6.7f, END = 8.8f;
  float floorOff = st.H - 3 - st.eyeH * 0.68f - st.CY;
  float wallOff  = st.W - st.eyeW * 0.55f - (st.CX + st.eyeOff);
  pose.blink = false;
  if (T < FALL) {                                      // it comes loose
    pose.rx = sinf(T * 90) * 2 * seg(T, 0, FALL);
    pose.mouth = MOUTH_DASH;
  } else if (T < HOPS) {
    float ax, ay;
    gravity(520, ax, ay);
    eye.vx += ax * dt * 0.25f; eye.vy += ay * dt;
    eye.x += eye.vx * dt; eye.y += eye.vy * dt;
    if (eye.x > wallOff) { eye.x = wallOff; eye.vx = -fabsf(eye.vx) * 0.5f; }
    if (eye.x < -st.eyeOff) { eye.x = -st.eyeOff; eye.vx = fabsf(eye.vx) * 0.5f; }
    if (eye.y > floorOff) {
      eye.y = floorOff;
      if (eye.vy > 60) eye.squashT = T;
      eye.vy = -fabsf(eye.vy) * 0.42f;
      eye.vx *= 0.7f;
      if (fabsf(eye.vy) < 25) { eye.vy = 0; eye.vx *= 0.8f; }
    }
    eye.restX = eye.x; eye.restY = eye.y;
    pose.rx = eye.x; pose.ry = eye.y;
    if (T - eye.squashT < 0.11f && eye.squashT >= 0) { pose.rsy = 0.68f; pose.rsx = 1.15f; }
    pose.lx = lim(eye.x * 0.1f, 8); pose.ly = lim(eye.y * 0.08f, 6);
    pose.mouth = MOUTH_O; pose.browRaise = 8; pose.browSlant = -0.5f;
    if (T > 1.6f && T < STARE) pose.badge = "!?";
    if (T > 2.9f && T < 3.02f) pose.lsy = 0.12f;       // blink...
    if (T > 3.2f && T < 3.32f) pose.rsy = 0.12f;       // ...it blinks back
  } else if (T < HOME) {                               // three hops home
    float u = (T - HOPS) / (HOME - HOPS) * 3;
    int   n = (int)u;
    float f = u - n;
    float from = n / 3.0f, to = (n + 1) / 3.0f;
    float q = from + (to - from) * smooth(f);
    float baseX = eye.restX * (1 - q), baseY = eye.restY * (1 - q);
    float lift = sinf(f * PI) * (st.H * 0.13f + (n == 2 ? 4 : 0));
    pose.rx = baseX; pose.ry = baseY - lift;
    if (f < 0.12f || f > 0.88f) { pose.rsy = 0.74f; pose.rsx = 1.12f; }
    pose.mouth = MOUTH_O; pose.browRaise = 3;
  } else if (T < END) {                                // pretends nothing happened
    float p = seg(T, HOME, HOME + 0.3f);
    pose.lsy = pose.rsy = 1 - 0.2f * sinf(p * PI);
    pose.gx = T < HOME + 0.8f ? -14.0f : T < HOME + 1.4f ? 14.0f : 0.0f;
    pose.happy = 0.35f * seg(T, HOME + 1.4f, HOME + 1.7f);
    pose.mouth = T > HOME + 1.4f ? MOUTH_SMILE : MOUTH_DASH;
    pose.blush = true;
    pose.badge = st.portrait ? "shh..." : "nobody saw that";
  } else {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// The fly: buzzes around, lands on an eye, then gets caught... usually
// ---------------------------------------------------------------------------
struct { float x, y, tx, ty, nextT, tongue; bool escapes, eaten; } fly;

void initFly() {
  fly.x = random(0, 2) ? -8.0f : st.W + 8.0f;
  fly.y = st.H * 0.2f;
  fly.tx = st.W / 2.0f; fly.ty = st.H * 0.3f; fly.nextT = 0;
  fly.escapes = random(0, 3) == 0;
  fly.eaten = false; fly.tongue = 0;
}

bool runFly() {
  const float LAND = 3.2f, TAKEOFF = 4.6f, CHASE = 6.4f, SNAP = 7.2f, CAUGHT = 7.5f, END = 9.3f;
  float buzz = fly.eaten ? 0 : 1;
  if (T < LAND || (T > TAKEOFF && T < CHASE)) {        // wanders
    if (T > fly.nextT) {
      fly.tx = rnd(14, st.W - 14); fly.ty = rnd(st.H * 0.08f, st.H * 0.8f);
      fly.nextT = T + rnd(0.35f, 0.8f);
    }
  } else if (T < TAKEOFF) {                            // lands on its eyebrow side
    fly.tx = st.CX - st.eyeOff + st.eyeW * 0.2f;
    fly.ty = st.CY - st.eyeH * 0.5f - 2;
    buzz = 0.25f;
  } else if (!fly.escapes) {                           // drifts in front of its mouth
    fly.tx = st.CX + 22 * st.S; fly.ty = st.mouthY - 2;
    buzz = 0.5f;
  } else {                                             // gets away
    fly.tx = st.W + 30; fly.ty = -20;
  }
  float k = min(1.0f, dt * (T > CHASE && fly.escapes ? 3.0f : 4.5f));
  fly.x += (fly.tx - fly.x) * k;
  fly.y += (fly.ty - fly.y) * k;
  fly.x += rnd(-1.6f, 1.6f) * buzz; fly.y += rnd(-1.6f, 1.6f) * buzz;

  pose.gx = lim((fly.x - st.CX) * 0.17f, 20);
  pose.gy = lim((fly.y - st.CY) * 0.14f, 7);
  pose.browRaise = 3;
  pose.mouth = MOUTH_DASH;
  pose.blink = false;

  if (T >= LAND && T < TAKEOFF) {                      // tickles
    pose.lsy = 1 - 0.1f * fabsf(sinf(T * 24)); pose.flat = 0.12f;
    pose.browSlant = -0.5f; pose.mouth = MOUTH_WAVY;
  }
  if (T >= CHASE) {
    if (fly.escapes) {
      pose.flat = 0.3f * seg(T, SNAP, SNAP + 0.3f); pose.browSlant = 0.7f * seg(T, SNAP, SNAP + 0.3f);
      pose.mouth = MOUTH_FROWN;
      if (T > SNAP) pose.badge = "hmph";
      if (T > SNAP) { pose.gx = pose.gy = 0; }
    } else {
      pose.browSlant = 0.6f; pose.mouth = T < SNAP ? MOUTH_DASH : MOUTH_YAWN;
      if (T >= SNAP && T < CAUGHT) {                   // tongue!
        float p = seg(T, SNAP, CAUGHT);
        fly.tongue = p < 0.5f ? p * 2 : (1 - p) * 2;
        if (p >= 0.5f) {
          fly.eaten = true;
          fly.x = st.CX + 22 * st.S * fly.tongue;
          fly.y = st.mouthY;
        }
      } else if (T >= CAUGHT) {
        fly.tongue = 0; fly.eaten = true;
        pose.mouth = T < END - 0.8f ? MOUTH_CHEW : MOUTH_SMILE;
        pose.happy = 0.5f; pose.browSlant = 0; pose.gx = pose.gy = 0;
        if (T > END - 1.2f) pose.badge = "yum!";
      }
    }
  }
  return T < END;
}

void drawFly(M5Canvas* c, unsigned long now) {
  if (fly.tongue > 0.01f) {
    int tx = st.CX + (int)(22 * st.S * fly.tongue);
    c->drawWideLine(st.CX, st.mouthY + (int)(2 * st.S), tx, st.mouthY, 1.6f, ST_BABY);
  }
  if (fly.eaten && T >= 7.5f) return;
  int x = (int)fly.x, y = (int)fly.y;
  int wing = (now / 45) % 2 ? 1 : 2;
  c->fillEllipse(x - 3, y - wing, 2, 1, dim(BROW, 0.8f));
  c->fillEllipse(x + 3, y - wing, 2, 1, dim(BROW, 0.8f));
  c->fillCircle(x, y, 2, GREY);
  c->drawPixel(x - 1, y - 1, TEXT);
}

// ---------------------------------------------------------------------------
// WiFi snack: as many snacks as there are networks around (up to 3)
// ---------------------------------------------------------------------------
struct { float x, y, faceX, eatT; int left; bool falling; char label[16]; } snack;

void newSnack() {
  snack.x = rnd(st.W * 0.3f, st.W * 0.7f);
  snack.y = -12;
  snack.falling = true;
}

void initSnack() {
  snack.left = constrain(wifiStats.netCount, 1, 3);
  snack.faceX = 0; snack.eatT = -9;
  snprintf(snack.label, sizeof(snack.label), st.portrait ? "%d nets" : "%d networks!", wifiStats.netCount);
  newSnack();
}

bool runSnack() {
  pose.blink = false;
  float mouthY = st.mouthY;
  if (snack.falling) {
    snack.y += 62 * sqrtf(st.H / 135.0f) * dt;
    float wantX = lim(snack.x - st.CX, st.W - (st.CX + st.eyeOff + st.eyeW * 0.6f));
    snack.faceX += (wantX - snack.faceX) * min(1.0f, dt * 4);
    pose.gx = lim((snack.x - (st.CX + snack.faceX)) * 0.2f, 16);
    pose.gy = lim((snack.y - st.CY) * 0.12f, 7);
    pose.browRaise = 5;
    bool close = snack.y > mouthY - st.H * 0.42f;
    pose.mouth = close ? MOUTH_YAWN : MOUTH_O;
    if (snack.y >= mouthY - 4 && fabsf(snack.x - (st.CX + snack.faceX)) < 30) {
      snack.falling = false; snack.eatT = T; snack.left--;
    } else if (snack.y > st.H + 14) {
      snack.falling = false; snack.eatT = T; snack.left--;     // missed it: never stuck
    }
  } else {
    float since = T - snack.eatT;
    pose.mouth = MOUTH_CHEW; pose.happy = 0.45f; pose.browRaise = 3;
    if (since > 0.9f) {
      if (snack.left > 0) newSnack();
      else {
        pose.mouth = since < 1.1f ? MOUTH_O : MOUTH_SMILE;
        if (since > 1.0f) pose.badge = "*burp*";
        if (since > 2.3f) return false;
      }
    }
  }
  if (T < 1.6f) pose.badge = snack.label;
  pose.ox = snack.faceX;
  if (!snack.falling) snack.faceX *= max(0.0f, 1 - dt * 4);
  return T < 22;
}

void drawSnack(M5Canvas* c) {
  if (!snack.falling) return;
  int x = (int)snack.x, y = (int)snack.y + 6;
  for (int i = 0; i < 3; i++) c->fillArc(x, y, 3 + i * 4, 4 + i * 4, 225, 315, SEC_WIFI);
  c->fillCircle(x, y, 2, SEC_WIFI);
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------
bool busy(unsigned long now) {
  if (currentScreen != SCREEN_HOME || currentActivity != ACT_NONE || restPhase != REST_NONE) return true;
  if (wifiScanInProgress || hungerEffectActive || dancing || listening || walking || rockDrowsy > 0.05f) return true;
  if (now < fallUntil || now < ouchUntil || now < wheeUntil || now < dizzyUntil || now < calledUntil ||
      now < zapUntil || now < giggleUntil || now < grumpyUntil || now < noticeUntil || now < startleUntil ||
      now < stirUntil || now < playEffectUntil || now < stepGoalUntil) return true;
  return false;
}

bool tooUnwell() {
  return currentMood == MOOD_SICK || runningHot ||
         (powerState == PWR_BATTERY && batteryShown >= 0 && batteryShown <= 15);
}

void scheduleNext(unsigned long now) {
  uint32_t lo = idleShows == SHOWS_CALM ? CALM_MIN : LIVELY_MIN;
  uint32_t hi = idleShows == SHOWS_CALM ? CALM_MAX : LIVELY_MAX;
  nextDue = now + random(lo, hi);
}

ShowId pick() {
  int w[SHOW_COUNT] = { 0, 3, 4, 3, 3, 3, 3 };
  if (pet.hunger < 50)    { w[SHOW_SNACK] += 5; w[SHOW_FLY] += 3; w[SHOW_BALL] = 1; }
  if (pet.happiness > 70) { w[SHOW_BALL] += 2; w[SHOW_CUBE] += 2; }
  if (wifiStats.netCount == 0) w[SHOW_SNACK] = 0;
  for (ShowId r : recent) w[r] = 0;
  int total = 0;
  for (int i = 1; i < SHOW_COUNT; i++) total += w[i];
  if (total <= 0) return SHOW_PEEK;
  int r = random(0, total);
  for (int i = 1; i < SHOW_COUNT; i++) { r -= w[i]; if (r < 0) return (ShowId)i; }
  return SHOW_PEEK;
}

void begin(ShowId id, unsigned long now) {
  current = id; startMs = now; T = 0;
  recent[2] = recent[1]; recent[1] = recent[0]; recent[0] = id;
  switch (id) {
    case SHOW_BALL:  initBall();  break;
    case SHOW_EYE:   initEye();   break;
    case SHOW_FLY:   initFly();   break;
    case SHOW_SNACK: initSnack(); break;
    default: break;
  }
  Serial.printf("SHOW-EV start %d\n", (int)id);
}

void finish(const char* how, unsigned long now) {
  Serial.printf("SHOW-EV %s %d after %.1fs\n", how, (int)current, T);
  current = SHOW_NONE;
  pose = ShowPose();
  scheduleNext(now);
}

}  // namespace

void showsTick(unsigned long now, const ShowStage& stage) {
  st = stage;
  dt = lastTick ? constrain((now - lastTick) / 1000.0f, 0.001f, 0.05f) : 0.03f;
  bool gap = lastTick && now - lastTick > 500;         // the home screen was away
  lastTick = now;

  if (current != SHOW_NONE) {
    if (gap) { finish("away", now); return; }
    if (fabsf(faceTiltDelta) > TURNED_DEG) { finish("turned", now); return; }
    if (busy(now)) { oopsPending = true; finish("interrupted", now); return; }

    T = (now - startMs) / 1000.0f;
    pose = ShowPose();
    pose.on = true;
    pose.mouth = st.restMouth;
    bool more = true;
    switch (current) {
      case SHOW_PEEK:  more = runPeek();  break;
      case SHOW_BALL:  more = runBall();  break;
      case SHOW_CUBE:  more = runCube();  break;
      case SHOW_EYE:   more = runEye();   break;
      case SHOW_FLY:   more = runFly();   break;
      case SHOW_SNACK: more = runSnack(); break;
      default: more = false;
    }
    if (!more) finish("end", now);
    return;
  }

  // Interrupted: your reaction first, then a shy little "oops"
  if (oopsPending && !busy(now)) { oopsPending = false; oopsUntil = now + 1500; }
  if (now < oopsUntil) {
    if (busy(now)) { oopsUntil = 0; pose = ShowPose(); return; }
    pose = ShowPose();
    pose.on = true; pose.gx = 18; pose.gy = 6; pose.blush = true; pose.happy = 0.2f;
    pose.mouth = MOUTH_SMILE; pose.badge = "oops..";
    return;
  }
  pose.on = false;

  if (nextDue == 0) nextDue = now + FIRST_SHOW;
  if (idleShows == SHOWS_OFF || now < nextDue) return;
  if (busy(now) || tooUnwell()) { nextDue = now + BUSY_RETRY; return; }
  begin(pick(), now);
}

void showsDraw(M5Canvas* c, bool front) {
  if (current == SHOW_NONE) return;
  if (!front) return;                                  // every prop sits in front of the face for now
  switch (current) {
    case SHOW_BALL:  drawBall(c);  break;
    case SHOW_CUBE:  drawCube(c);  break;
    case SHOW_FLY:   drawFly(c, millis()); break;
    case SHOW_SNACK: drawSnack(c); break;
    default: break;
  }
}

const ShowPose& showsPose() { return pose; }
bool showsRunning()         { return current != SHOW_NONE; }

bool showsStart(ShowId id) {
  unsigned long now = millis();
  if (id <= SHOW_NONE || id >= SHOW_COUNT || current != SHOW_NONE || busy(now)) {
    Serial.printf("SHOW-EV refused %d\n", (int)id);
    return false;
  }
  begin(id, now);
  return true;
}
