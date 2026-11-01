// DSMR/P1 telegram parser for Israeli IEC Kaifa meters (MA309M 3-phase,
// MA110M 1-phase). Byte-by-byte, CRC16 checked, no heap allocations.
// Pure C++ (no Arduino), so it is unit-tested on the PC (test/host).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Bits in MeterReading::present
enum : uint32_t {
  HAS_IMPORT = 1u << 0,   // 1-0:1.8.11..13
  HAS_EXPORT = 1u << 1,   // 1-0:2.8.11..13
  HAS_POWER = 1u << 2,    // 1-0:1.7.0
  HAS_POWER_EXP = 1u << 3,// 1-0:2.7.0
  HAS_TARIFF = 1u << 4,   // 0-0:96.14.1
  HAS_TIME = 1u << 5,     // 0-0:1.0.0
  HAS_L1 = 1u << 8,       // any of voltage/current/power on L1
  HAS_L2 = 1u << 9,
  HAS_L3 = 1u << 10,
  HAS_PL1 = 1u << 12,     // per-phase active power (3-phase meters only)
  HAS_PL2 = 1u << 13,
  HAS_PL3 = 1u << 14,
};

struct MeterReading {
  uint32_t present = 0;
  uint32_t imp_Wh[3] = {0, 0, 0};  // energy imported, tariffs T1..T3 (Wh)
  uint32_t exp_Wh[3] = {0, 0, 0};  // energy exported to the grid (Wh)
  uint32_t p_imp_W = 0;            // total active power drawn (W)
  uint32_t p_exp_W = 0;            // total active power returned (W)
  uint32_t pl_W[3] = {0, 0, 0};    // per-phase power drawn (W)
  uint32_t ple_W[3] = {0, 0, 0};   // per-phase power returned (W)
  uint16_t v_dV[3] = {0, 0, 0};    // per-phase voltage (0.1 V)
  uint16_t i_cA[3] = {0, 0, 0};    // per-phase current (0.01 A)
  uint8_t tariff = 0;
  char meter_time[16] = {0};       // YYMMDDhhmmssX, X = S (summer) / W (winter)
  char meter_id[32] = {0};         // 0-0:96.1.1 / 0-0:96.1.0
  char header[40] = {0};           // first line, e.g. /KFM5KAIFA-METER

  uint32_t importTotalWh() const { return imp_Wh[0] + imp_Wh[1] + imp_Wh[2]; }
  uint32_t exportTotalWh() const { return exp_Wh[0] + exp_Wh[1] + exp_Wh[2]; }
  int phases() const {
    return (present & HAS_L3) ? 3 : (present & HAS_L2) ? 2 : (present & HAS_L1) ? 1 : 0;
  }
};

struct P1Stats {
  uint32_t ok = 0;
  uint32_t crc_errors = 0;
  uint32_t overflows = 0;   // line too long / garbage
  uint32_t bytes = 0;
};

class P1Parser {
 public:
  // Feeds one byte. Returns true when a complete, CRC-valid telegram has been
  // parsed; the result is then available in reading().
  bool feed(uint8_t c);
  const MeterReading &reading() const { return done_; }
  const P1Stats &stats() const { return stats_; }
  void reset() { state_ = IDLE; }

  // Helpers, public for the unit tests
  static uint16_t crc16(uint16_t crc, uint8_t c);
  // Parses "001234.567*kWh" into an integer with `decimals` decimals:
  // ("001234.567", 3) -> 1234567. Returns false if there are no digits.
  static bool parseFixed(const char *s, int decimals, int64_t *out);

 private:
  enum State { IDLE, BODY, FOOTER };
  void parseLine(const char *line, size_t len);

  State state_ = IDLE;
  uint16_t crc_ = 0;
  char line_[256];
  size_t line_len_ = 0;
  bool line_overflow_ = false;
  bool first_line_ = false;
  char footer_[8];
  size_t footer_len_ = 0;
  MeterReading work_;
  MeterReading done_;
  P1Stats stats_;
};
