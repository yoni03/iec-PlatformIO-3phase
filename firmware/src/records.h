// History records and the aggregation from telegrams -> minutes -> hours -> days.
// Pure C++ (no Arduino), unit-tested on the PC.
#pragma once
#include <stdint.h>

#include "p1_parser.h"

// One history record. Same layout for the minute, hour and day files and for
// the binary /api/history response (little-endian, 52 bytes).
struct Rec {
  uint32_t ts;          // period start, unix time (UTC seconds)
  uint32_t imp_end;     // meter import counter at the end of the period (Wh)
  uint32_t exp_end;     // meter export counter at the end of the period (Wh)
  uint32_t imp_Wh;      // energy drawn during the period (Wh)
  uint32_t exp_Wh;      // energy returned during the period (Wh)
  uint32_t cost_u;      // cost of imp_Wh in micro-ILS, with the discount that applied
  uint16_t p_avg;       // average power drawn (W)
  uint16_t p_max;       // highest power (W)
  uint16_t p_min;       // minute: lowest sample. hour/day: lowest 1-minute average (baseload)
  uint16_t pl_avg[3];   // average power per phase (W)
  uint16_t v_avg[3];    // average voltage per phase (0.1 V)
  uint16_t i_max[3];    // highest current per phase (0.01 A)
  uint16_t samples;     // minute: telegrams; hour/day: child records
  uint8_t tariff;       // meter tariff at the end of the period
  uint8_t flags;        // REC_*
};
static_assert(sizeof(Rec) == 52, "Rec must stay 52 bytes, it is the file format");

enum : uint8_t {
  REC_COUNTERS = 1,   // energy from the meter's counters (exact)
  REC_ESTIMATED = 2,  // energy estimated from power (meter sent no counters)
  REC_DEMO = 4,       // produced in demo mode
};

// Accumulates telegrams during one minute
class MinuteAgg {
 public:
  void add(const MeterReading &r);
  bool empty() const { return n_ == 0; }
  // Builds the record. `prev` is the previous minute record, or nullptr.
  // Energy is taken from the counters: end - (prev.imp_end if contiguous, else
  // the first counter seen in this minute).
  Rec finish(uint32_t minute_ts, const Rec *prev, uint32_t price_u, bool demo) const;
  void clear() { *this = MinuteAgg(); }
  // Largest believable energy in one minute (120 kW); bigger jumps = counter reset
  static const uint32_t kMaxMinuteWh = 2000;

 private:
  uint32_t n_ = 0;
  uint64_t p_sum_ = 0;
  uint32_t p_max_ = 0, p_min_ = UINT32_MAX;
  uint64_t pl_sum_[3] = {0, 0, 0};
  uint64_t v_sum_[3] = {0, 0, 0};
  uint32_t v_n_[3] = {0, 0, 0};
  uint16_t i_max_[3] = {0, 0, 0};
  bool counters_ = false;
  uint32_t imp_first_ = 0, imp_last_ = 0, exp_first_ = 0, exp_last_ = 0;
  uint8_t tariff_ = 0;
};

// Combines minute records into an hour/day record, or hour records into a day.
class RecAgg {
 public:
  explicit RecAgg(bool children_are_minutes) : minutes_(children_are_minutes) {}
  // Energy between two children (device was off) is added if it is plausible
  static const uint32_t kMaxGapW = 60000;
  void add(const Rec &r);
  // Starts a new period from the last child of the previous period, so energy
  // used while the device was off across the boundary is counted here
  void seed(const Rec &prev_child);
  bool empty() const { return n_ == 0; }
  uint32_t count() const { return n_; }
  // price_u is used only for energy that fell into gaps between children
  Rec finish(uint32_t ts, uint32_t price_u) const;
  // Peak of the period and the time of the child record where it happened
  uint32_t peakTs() const { return peak_ts_; }
  void clear() { *this = RecAgg(minutes_); }

 private:
  bool minutes_;
  uint32_t n_ = 0;
  uint64_t imp_sum_ = 0, exp_sum_ = 0, cost_sum_ = 0, p_sum_ = 0;
  uint64_t pl_sum_[3] = {0, 0, 0};
  uint64_t v_sum_[3] = {0, 0, 0};
  uint32_t v_n_[3] = {0, 0, 0};
  uint16_t i_max_[3] = {0, 0, 0};
  uint16_t p_max_ = 0, p_min_ = UINT16_MAX;
  uint32_t peak_ts_ = 0;
  bool counters_ = false;
  uint32_t last_ts_ = 0;
  uint32_t imp_end_ = 0, exp_end_ = 0;
  uint64_t gap_imp_ = 0, gap_exp_ = 0;
  uint8_t tariff_ = 0, flags_ = 0;
};

inline uint32_t costMicro(uint32_t wh, uint32_t price_u_per_kwh) {
  return (uint32_t)(((uint64_t)wh * price_u_per_kwh + 500) / 1000);
}
