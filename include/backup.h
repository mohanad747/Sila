#pragma once
#include <stdint.h>
#include <stddef.h>

// 64-byte pet record kept outside the firmware area (see backup.cpp)
struct __attribute__((packed)) SaveBlob {
  uint32_t magic;
  uint32_t seq;
  int16_t  hunger, happiness, health;
  uint32_t ageMin, ageHr, ageDay;
  uint8_t  stage, hatched;
  uint8_t  tCur, tAct, tStr;
  uint32_t stToday, stTotal, stBest;
  uint16_t stDay;
  uint8_t  soundMode, bright, volume;
  uint8_t  flags;               // bit0 autoRotate, 1 lieDown, 2 ears, 3 radar, 4 scanSaver, 5 flipped
  uint8_t  idleMode;
  uint16_t gameBest;            // v3.7: best Catch score (0 in older backups)
  uint8_t  reserved[8];
  uint32_t crc;
};
static_assert(sizeof(SaveBlob) == 64, "SaveBlob must be 64 bytes");

bool backupLoad(SaveBlob& out);
void backupWrite(SaveBlob b);
