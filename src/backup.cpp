// Sila - pet backup that survives re-flashing.
//
// The normal save lives in NVS (Preferences) at 0x9000, which a full firmware
// image flashed at 0x0 overwrites. So a compact copy of the pet is also kept in
// the "spiffs" data partition (0x670000 on the 8 MB layout), far beyond the end
// of the firmware image. After a flash, if NVS is empty, the pet is restored
// from here.
//
// Wear levelling: 64-byte records are appended into a ring of 16 flash sectors
// (64 per sector), so each sector is erased only once every ~1000 backups.

#include <Arduino.h>
#include <esp_partition.h>
#include "backup.h"

static const uint32_t MAGIC        = 0x53494C41;   // "SILA"
static const int      RING_SECTORS = 16;
static const int      SECTOR       = 4096;
static const int      SLOTS        = SECTOR / sizeof(SaveBlob);

static const esp_partition_t* part = nullptr;
static int  curSector = -1, curSlot = 0;
static uint32_t lastSeq = 0;

static uint32_t checksum(const SaveBlob& b) {
  const uint8_t* p = (const uint8_t*)&b;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < offsetof(SaveBlob, crc); i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static bool ensurePartition() {
  if (part) return true;
  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  return part && part->size >= (size_t)RING_SECTORS * SECTOR;
}

// Scan the ring: find the newest valid record and where to write next
bool backupLoad(SaveBlob& out) {
  if (!ensurePartition()) return false;
  bool found = false;
  SaveBlob b;
  int bestSec = -1, bestSlot = 0;
  for (int s = 0; s < RING_SECTORS; s++) {
    for (int i = 0; i < SLOTS; i++) {
      if (esp_partition_read(part, s * SECTOR + i * sizeof(SaveBlob), &b, sizeof(b)) != ESP_OK) continue;
      if (b.magic != MAGIC || b.crc != checksum(b)) continue;
      if (!found || b.seq > lastSeq) { out = b; lastSeq = b.seq; found = true; bestSec = s; bestSlot = i; }
    }
  }
  if (found) { curSector = bestSec; curSlot = bestSlot + 1; }
  else       { curSector = -1; curSlot = 0; }
  return found;
}

void backupWrite(SaveBlob b) {
  if (!ensurePartition()) return;
  if (curSector < 0) { SaveBlob tmp; backupLoad(tmp); }

  b.magic = MAGIC;
  b.seq   = ++lastSeq;
  b.crc   = checksum(b);

  for (int attempt = 0; attempt < 3; attempt++) {
    if (curSector < 0 || curSlot >= SLOTS) {             // start a fresh sector
      curSector = (curSector + 1 + RING_SECTORS) % RING_SECTORS;
      curSlot = 0;
      esp_partition_erase_range(part, curSector * SECTOR, SECTOR);
    }
    size_t off = curSector * SECTOR + curSlot * sizeof(SaveBlob);
    uint32_t probe = 0;
    esp_partition_read(part, off, &probe, sizeof(probe));
    if (probe != 0xFFFFFFFF) { curSlot = SLOTS; continue; } // not blank: move on
    if (esp_partition_write(part, off, &b, sizeof(b)) == ESP_OK) { curSlot++; return; }
    curSlot = SLOTS;
  }
}
