#pragma once
// Sila - colour language.
//
// Every colour means something:
//
//   TEAL        = YOU (Mohanad). Anything you control: selection highlights,
//                 the Controls section, button hints you act on.
//   TERRACOTTA  = THE PET's voice. Hints, speech badges,
//                 messages - when the pet "talks" to you it is in this colour.
//
//   Stats       HUNGER = amber (food)   HAPPY = pink (heart)   HEALTH = green (life)
//   Sections    VITALS = heartbeat red  WALKS = lime (movement)  PET = violet (identity)
//               WIFI/RADAR = sky blue (air)  PEOPLE = pink (heart)
//               CONTROLS = teal (you)   SETTINGS = sun yellow (tuning)
//               SYSTEM = slate (the machine)  HOME = terracotta (back to the pet)
//               PLAY = warm orange (fun)
//   Stages      BABY = pink  TEEN = teal  ADULT = blue  ELDER = violet
//   Status      GOOD = green  WARN = amber  BAD = red

#include <M5Unified.h>

namespace theme {
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color565(r, g, b); }

// ---- Base ----
constexpr uint16_t BG       = rgb(0, 0, 0);
constexpr uint16_t PANEL    = rgb(16, 22, 28);     // content panel fill
constexpr uint16_t TEXT     = rgb(235, 245, 250);
constexpr uint16_t GREY     = rgb(125, 140, 150);  // secondary text
constexpr uint16_t SEG_OFF  = rgb(38, 48, 56);     // empty bar segments, dividers

// ---- Us ----
constexpr uint16_t TEAL     = rgb(40, 220, 205);   // you
constexpr uint16_t TEAL_DIM = rgb(14, 64, 62);
constexpr uint16_t TERRA   = rgb(217, 119, 87);   // the pet's voice (#D97757)

// ---- Stats ----
constexpr uint16_t C_HUNGER = rgb(255, 172, 40);
constexpr uint16_t C_HAPPY  = rgb(255, 105, 175);
constexpr uint16_t C_HEALTH = rgb(90, 228, 120);

// ---- Sections ----
constexpr uint16_t SEC_VITALS   = rgb(255, 84, 104);
constexpr uint16_t SEC_PET      = rgb(176, 128, 255);
constexpr uint16_t SEC_WALK     = rgb(176, 232, 60);    // movement (lime)
constexpr uint16_t C_PEOPLE     = rgb(255, 105, 175);   // people nearby (heart pink)
constexpr uint16_t SEC_WIFI     = rgb(72, 164, 255);
constexpr uint16_t SEC_CONTROLS = TEAL;
constexpr uint16_t SEC_SETTINGS = rgb(255, 208, 70);
constexpr uint16_t SEC_SYSTEM   = rgb(150, 172, 192);
constexpr uint16_t SEC_HOME     = TERRA;
constexpr uint16_t SEC_PLAY     = rgb(255, 140, 60);    // play (warm orange)

// ---- Stages ----
constexpr uint16_t ST_BABY  = rgb(255, 150, 200);
constexpr uint16_t ST_TEEN  = TEAL;
constexpr uint16_t ST_ADULT = rgb(96, 156, 255);
constexpr uint16_t ST_ELDER = rgb(186, 146, 255);

// ---- Status ----
constexpr uint16_t GOOD  = C_HEALTH;
constexpr uint16_t AMBER = rgb(255, 160, 50);
constexpr uint16_t ALERT = rgb(255, 64, 72);

// ---- Face ----
constexpr uint16_t EYE       = rgb(232, 248, 255);
constexpr uint16_t EYE_GLOW  = rgb(22, 52, 66);    // default halo (teal family)
constexpr uint16_t BROW      = rgb(140, 200, 235);
constexpr uint16_t EYE_SICK  = rgb(150, 215, 120);
constexpr uint16_t EYE_SLEEP = rgb(110, 160, 200);
constexpr uint16_t BLUSH     = rgb(110, 45, 60);
constexpr uint16_t HEART     = C_HAPPY;

// Mix a colour toward black (f = 0..1 of the original kept)
inline uint16_t dim(uint16_t c, float f) {
  uint8_t r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
  return rgb((uint8_t)(r * f), (uint8_t)(g * f), (uint8_t)(b * f));
}
}  // namespace theme
