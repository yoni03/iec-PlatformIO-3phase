// History files on LittleFS. Append-only, one file per period:
//   /m/YYYYMMDD.bin  minute records of one local day
//   /h/YYYYMM.bin    hour records of one local month
//   /d/YYYY.bin      day records of one local year
// Every file starts with an 8 byte header (magic, version, record size).
#pragma once
#include <Arduino.h>

#include <functional>

#include "records.h"

enum Tier { TIER_MINUTE = 0, TIER_HOUR = 1, TIER_DAY = 2 };

namespace storage {
bool begin();
void append(Tier tier, const Rec *recs, size_t n);
// Last record of the tier (newest file). False if there is none.
bool last(Tier tier, Rec *out);
// Calls fn for every record with from <= ts < to, oldest first. fn returns
// false to stop. `idle` is called between files/chunks (to keep reading the meter).
void forEach(Tier tier, uint32_t from, uint32_t to, const std::function<bool(const Rec &)> &fn,
             const std::function<void()> &idle = nullptr);
void prune();       // retention by file count + emergency pruning when nearly full
void clearAll();    // deletes all history
size_t usedBytes();
size_t totalBytes();
int fileCount(Tier tier);
Tier tierFromChar(char c);
}  // namespace storage
