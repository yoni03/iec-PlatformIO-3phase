#include "storage.h"

#include <LittleFS.h>
#include <time.h>

#include <algorithm>
#include <vector>

#include "config.h"

namespace {

const char *kDirs[3] = {"/m", "/h", "/d"};
const int kKeep[3] = {KEEP_MINUTE_FILES, KEEP_HOUR_FILES, KEEP_DAY_FILES};

struct Header {
  char magic[4];
  uint16_t version;
  uint16_t rec_size;
};
static_assert(sizeof(Header) == 8, "header size");
const Header kHeader = {{'I', 'E', 'C', 'R'}, 1, sizeof(Rec)};

String fileFor(Tier tier, uint32_t ts) {
  time_t t = ts;
  struct tm lt;
  localtime_r(&t, &lt);
  char name[24];
  switch (tier) {
    case TIER_MINUTE: snprintf(name, sizeof(name), "/m/%04d%02d%02d.bin", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday); break;
    case TIER_HOUR: snprintf(name, sizeof(name), "/h/%04d%02d.bin", lt.tm_year + 1900, lt.tm_mon + 1); break;
    default: snprintf(name, sizeof(name), "/d/%04d.bin", lt.tm_year + 1900); break;
  }
  return String(name);
}

// File names of a tier, sorted oldest first (names sort chronologically)
std::vector<String> listFiles(Tier tier) {
  std::vector<String> names;
  File dir = LittleFS.open(kDirs[tier]);
  if (!dir || !dir.isDirectory()) return names;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String n = f.name();
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    if (!f.isDirectory() && n.endsWith(".bin")) names.push_back(n);
    f.close();
  }
  std::sort(names.begin(), names.end(), [](const String &a, const String &b) { return a < b; });
  return names;
}

// Local time range [start, end) covered by a file name such as 20261101.bin
bool fileRange(Tier tier, const String &name, uint32_t *start, uint32_t *end) {
  struct tm lt = {};
  const int y = name.substring(0, 4).toInt();
  if (y < 2000) return false;
  lt.tm_year = y - 1900;
  lt.tm_mon = tier == TIER_DAY ? 0 : name.substring(4, 6).toInt() - 1;
  lt.tm_mday = tier == TIER_MINUTE ? name.substring(6, 8).toInt() : 1;
  lt.tm_isdst = -1;
  time_t s = mktime(&lt);
  if (tier == TIER_MINUTE) lt.tm_mday += 1;
  else if (tier == TIER_HOUR) lt.tm_mon += 1;
  else lt.tm_year += 1;
  lt.tm_isdst = -1;
  time_t e = mktime(&lt);
  if (s <= 0 || e <= s) return false;
  *start = (uint32_t)s;
  *end = (uint32_t)e;
  return true;
}

bool headerOk(File &f) {
  Header h;
  if (f.read((uint8_t *)&h, sizeof(h)) != sizeof(h)) return false;
  return memcmp(h.magic, kHeader.magic, 4) == 0 && h.rec_size == sizeof(Rec);
}

}  // namespace

namespace storage {

bool begin() {
  if (!LittleFS.begin(true)) {  // formats the partition on first use
    Serial.println("[fs] LittleFS mount failed");
    return false;
  }
  for (const char *d : kDirs)
    if (!LittleFS.exists(d)) LittleFS.mkdir(d);
  Serial.printf("[fs] %u / %u bytes used\n", (unsigned)usedBytes(), (unsigned)totalBytes());
  return true;
}

void append(Tier tier, const Rec *recs, size_t n) {
  size_t i = 0;
  while (i < n) {
    // Records that go to the same file are written together
    const String path = fileFor(tier, recs[i].ts);
    size_t j = i + 1;
    while (j < n && fileFor(tier, recs[j].ts) == path) j++;
    const bool exists = LittleFS.exists(path);
    File f = LittleFS.open(path, exists ? FILE_APPEND : FILE_WRITE);
    if (!f) {
      Serial.printf("[fs] cannot open %s\n", path.c_str());
      return;
    }
    if (!exists || f.size() == 0) f.write((const uint8_t *)&kHeader, sizeof(kHeader));
    f.write((const uint8_t *)&recs[i], sizeof(Rec) * (j - i));
    f.close();
    i = j;
  }
}

bool last(Tier tier, Rec *out) {
  std::vector<String> files = listFiles(tier);
  for (int k = (int)files.size() - 1; k >= 0; k--) {
    File f = LittleFS.open(String(kDirs[tier]) + "/" + files[k], FILE_READ);
    if (!f) continue;
    const size_t size = f.size();
    if (size >= sizeof(Header) + sizeof(Rec) && headerOk(f)) {
      const size_t count = (size - sizeof(Header)) / sizeof(Rec);
      f.seek(sizeof(Header) + (count - 1) * sizeof(Rec));
      const bool ok = f.read((uint8_t *)out, sizeof(Rec)) == sizeof(Rec);
      f.close();
      if (ok) return true;
    }
    f.close();
  }
  return false;
}

void forEach(Tier tier, uint32_t from, uint32_t to, const std::function<bool(const Rec &)> &fn,
             const std::function<void()> &idle) {
  std::vector<String> files = listFiles(tier);
  Rec buf[24];
  for (const String &name : files) {
    uint32_t fs, fe;
    if (!fileRange(tier, name, &fs, &fe)) continue;
    if (fe <= from || fs >= to) continue;
    File f = LittleFS.open(String(kDirs[tier]) + "/" + name, FILE_READ);
    if (!f) continue;
    if (!headerOk(f)) {
      f.close();
      continue;
    }
    while (true) {
      const int got = f.read((uint8_t *)buf, sizeof(buf)) / (int)sizeof(Rec);
      if (got <= 0) break;
      for (int k = 0; k < got; k++) {
        if (buf[k].ts >= to) {  // records are in time order: nothing more to read
          f.close();
          return;
        }
        if (buf[k].ts >= from && !fn(buf[k])) {
          f.close();
          return;
        }
      }
      if (idle) idle();
    }
    f.close();
  }
}

int fileCount(Tier tier) { return (int)listFiles(tier).size(); }

void prune() {
  for (int t = 0; t < 3; t++) {
    std::vector<String> files = listFiles((Tier)t);
    for (int k = 0; k + kKeep[t] < (int)files.size(); k++) {
      LittleFS.remove(String(kDirs[t]) + "/" + files[k]);
      Serial.printf("[fs] retention: removed %s/%s\n", kDirs[t], files[k].c_str());
    }
  }
  // Emergency: free space by dropping the oldest minute, then hour, files
  for (int t = 0; t < 2 && usedBytes() * 100 > totalBytes() * FS_PRUNE_PERCENT; t++) {
    std::vector<String> files = listFiles((Tier)t);
    for (size_t k = 0; k + 2 < files.size() && usedBytes() * 100 > totalBytes() * FS_PRUNE_PERCENT; k++) {
      LittleFS.remove(String(kDirs[t]) + "/" + files[k]);
      Serial.printf("[fs] space: removed %s/%s\n", kDirs[t], files[k].c_str());
    }
  }
}

void clearAll() {
  for (int t = 0; t < 3; t++)
    for (const String &n : listFiles((Tier)t)) LittleFS.remove(String(kDirs[t]) + "/" + n);
}

size_t usedBytes() { return LittleFS.usedBytes(); }
size_t totalBytes() { return LittleFS.totalBytes(); }

Tier tierFromChar(char c) { return c == 'h' ? TIER_HOUR : c == 'd' ? TIER_DAY : TIER_MINUTE; }

}  // namespace storage
