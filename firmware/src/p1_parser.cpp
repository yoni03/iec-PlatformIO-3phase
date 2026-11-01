#include "p1_parser.h"

#include <stdlib.h>
#include <string.h>

uint16_t P1Parser::crc16(uint16_t crc, uint8_t c) {
  // CRC-16/ARC (poly 0x8005 reflected), as required by DSMR 4+/5
  crc ^= c;
  for (int bit = 0; bit < 8; bit++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  return crc;
}

bool P1Parser::parseFixed(const char *s, int decimals, int64_t *out) {
  int64_t value = 0;
  bool any = false, neg = false, dot = false;
  int frac = 0;
  if (*s == '-') { neg = true; s++; }
  for (; *s; s++) {
    if (*s >= '0' && *s <= '9') {
      if (dot) {
        if (frac >= decimals) continue;  // extra precision is truncated
        frac++;
      }
      value = value * 10 + (*s - '0');
      any = true;
    } else if (*s == '.' && !dot) {
      dot = true;
    } else {
      break;  // '*' unit separator or ')'
    }
  }
  if (!any) return false;
  for (; frac < decimals; frac++) value *= 10;
  *out = neg ? -value : value;
  return true;
}

bool P1Parser::feed(uint8_t c) {
  stats_.bytes++;
  if (c == '/') {
    // A new telegram always starts with '/', even if the previous one was cut
    state_ = BODY;
    crc_ = crc16(0, c);
    line_[0] = '/';
    line_len_ = 1;
    line_overflow_ = false;
    first_line_ = true;
    work_ = MeterReading();
    return false;
  }
  switch (state_) {
    case IDLE:
      return false;

    case BODY:
      crc_ = crc16(crc_, c);
      if (c == '!') {
        state_ = FOOTER;
        footer_len_ = 0;
        return false;
      }
      if (c == '\n') {
        if (line_overflow_) {
          stats_.overflows++;
        } else {
          while (line_len_ && line_[line_len_ - 1] == '\r') line_len_--;
          line_[line_len_] = 0;
          parseLine(line_, line_len_);
        }
        line_len_ = 0;
        line_overflow_ = false;
        return false;
      }
      if (line_len_ < sizeof(line_) - 1) {
        line_[line_len_++] = (char)c;
      } else {
        line_overflow_ = true;
      }
      return false;

    case FOOTER: {
      if (c == '\r' || c == '\n' || footer_len_ >= 4) {
        state_ = IDLE;
        footer_[footer_len_] = 0;
        if (footer_len_ == 0) {
          // Old DSMR versions have no CRC. Accept, the parser is strict anyway.
        } else if (footer_len_ != 4 || (uint16_t)strtoul(footer_, nullptr, 16) != crc_) {
          stats_.crc_errors++;
          return false;
        }
        done_ = work_;
        stats_.ok++;
        return true;
      }
      const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
      if (!hex) {
        state_ = IDLE;
        stats_.crc_errors++;
        return false;
      }
      footer_[footer_len_++] = (char)c;
      return false;
    }
  }
  return false;
}

namespace {
struct Field {
  const char *obis;
  int kind;   // see below
  int index;  // tariff or phase
};
enum {
  F_IMP, F_EXP, F_P, F_PE, F_PL, F_PLE, F_V, F_I, F_TARIFF, F_TIME, F_ID,
};
const Field kFields[] = {
    {"1-0:1.8.11", F_IMP, 0},  {"1-0:1.8.12", F_IMP, 1},  {"1-0:1.8.13", F_IMP, 2},
    {"1-0:2.8.11", F_EXP, 0},  {"1-0:2.8.12", F_EXP, 1},  {"1-0:2.8.13", F_EXP, 2},
    // European DSMR names, for meters that use T1/T2 instead of 11/12
    {"1-0:1.8.1", F_IMP, 0},   {"1-0:1.8.2", F_IMP, 1},
    {"1-0:2.8.1", F_EXP, 0},   {"1-0:2.8.2", F_EXP, 1},
    {"1-0:1.7.0", F_P, 0},     {"1-0:2.7.0", F_PE, 0},
    {"1-0:21.7.0", F_PL, 0},   {"1-0:41.7.0", F_PL, 1},   {"1-0:61.7.0", F_PL, 2},
    {"1-0:22.7.0", F_PLE, 0},  {"1-0:42.7.0", F_PLE, 1},  {"1-0:62.7.0", F_PLE, 2},
    {"1-0:32.7.0", F_V, 0},    {"1-0:52.7.0", F_V, 1},    {"1-0:72.7.0", F_V, 2},
    {"1-0:31.7.0", F_I, 0},    {"1-0:51.7.0", F_I, 1},    {"1-0:71.7.0", F_I, 2},
    {"0-0:96.14.0", F_TARIFF, 0}, {"0-0:96.14.1", F_TARIFF, 0},
    {"0-0:1.0.0", F_TIME, 0},
    {"0-0:96.1.1", F_ID, 0},   {"0-0:96.1.0", F_ID, 0},
};

void copyValue(char *dst, size_t size, const char *src) {
  size_t n = 0;
  while (src[n] && src[n] != ')' && n < size - 1) {
    dst[n] = src[n];
    n++;
  }
  dst[n] = 0;
}
}  // namespace

void P1Parser::parseLine(const char *line, size_t len) {
  if (first_line_) {
    first_line_ = false;
    copyValue(work_.header, sizeof(work_.header), line);
    return;
  }
  if (len == 0) return;
  const char *paren = strchr(line, '(');
  if (!paren) return;
  const size_t obis_len = (size_t)(paren - line);
  const char *value = paren + 1;

  for (const Field &f : kFields) {
    if (strlen(f.obis) != obis_len || strncmp(f.obis, line, obis_len) != 0) continue;
    int64_t v = 0;
    const int phase_bit = f.index;
    switch (f.kind) {
      case F_IMP:
        if (parseFixed(value, 3, &v) && v >= 0) { work_.imp_Wh[f.index] = (uint32_t)v; work_.present |= HAS_IMPORT; }
        break;
      case F_EXP:
        if (parseFixed(value, 3, &v) && v >= 0) { work_.exp_Wh[f.index] = (uint32_t)v; work_.present |= HAS_EXPORT; }
        break;
      case F_P:
        if (parseFixed(value, 3, &v) && v >= 0) { work_.p_imp_W = (uint32_t)v; work_.present |= HAS_POWER; }
        break;
      case F_PE:
        if (parseFixed(value, 3, &v) && v >= 0) { work_.p_exp_W = (uint32_t)v; work_.present |= HAS_POWER_EXP; }
        break;
      case F_PL:
        if (parseFixed(value, 3, &v) && v >= 0) {
          work_.pl_W[f.index] = (uint32_t)v;
          work_.present |= (HAS_PL1 << phase_bit) | (HAS_L1 << phase_bit);
        }
        break;
      case F_PLE:
        if (parseFixed(value, 3, &v) && v >= 0) work_.ple_W[f.index] = (uint32_t)v;
        break;
      case F_V:
        if (parseFixed(value, 1, &v) && v >= 0 && v < 65535) {
          work_.v_dV[f.index] = (uint16_t)v;
          work_.present |= (HAS_L1 << phase_bit);
        }
        break;
      case F_I:
        if (parseFixed(value, 2, &v) && v >= 0 && v < 65535) {
          work_.i_cA[f.index] = (uint16_t)v;
          work_.present |= (HAS_L1 << phase_bit);
        }
        break;
      case F_TARIFF:
        if (parseFixed(value, 0, &v) && v >= 0 && v < 256) { work_.tariff = (uint8_t)v; work_.present |= HAS_TARIFF; }
        break;
      case F_TIME:
        copyValue(work_.meter_time, sizeof(work_.meter_time), value);
        if (strlen(work_.meter_time) >= 12) work_.present |= HAS_TIME;
        break;
      case F_ID:
        copyValue(work_.meter_id, sizeof(work_.meter_id), value);
        break;
    }
    return;
  }
}
