#include "records.h"

#include <string.h>

namespace {
uint16_t clamp16(uint64_t v) { return v > 65535 ? 65535 : (uint16_t)v; }
}  // namespace

void MinuteAgg::add(const MeterReading &r) {
  uint32_t p = r.p_imp_W;
  n_++;
  p_sum_ += p;
  if (p > p_max_) p_max_ = p;
  if (p < p_min_) p_min_ = p;
  const bool per_phase_power = r.present & HAS_PL1;
  for (int k = 0; k < 3; k++) {
    // 1-phase meters have no per-phase power: the total is L1
    uint32_t pl = per_phase_power ? r.pl_W[k] : (k == 0 ? p : 0);
    pl_sum_[k] += pl;
    if (r.v_dV[k]) { v_sum_[k] += r.v_dV[k]; v_n_[k]++; }
    if (r.i_cA[k] > i_max_[k]) i_max_[k] = r.i_cA[k];
  }
  if (r.present & HAS_IMPORT) {
    const uint32_t imp = r.importTotalWh(), exp = r.exportTotalWh();
    if (!counters_) { imp_first_ = imp; exp_first_ = exp; counters_ = true; }
    imp_last_ = imp;
    exp_last_ = exp;
  }
  if (r.present & HAS_TARIFF) tariff_ = r.tariff;
}

Rec MinuteAgg::finish(uint32_t minute_ts, const Rec *prev, uint32_t price_u, bool demo) const {
  Rec rec;
  memset(&rec, 0, sizeof(rec));
  rec.ts = minute_ts;
  if (n_ == 0) return rec;
  rec.p_avg = clamp16(p_sum_ / n_);
  rec.p_max = clamp16(p_max_);
  rec.p_min = clamp16(p_min_);
  for (int k = 0; k < 3; k++) {
    rec.pl_avg[k] = clamp16(pl_sum_[k] / n_);
    rec.v_avg[k] = v_n_[k] ? clamp16(v_sum_[k] / v_n_[k]) : 0;
    rec.i_max[k] = i_max_[k];
  }
  rec.samples = clamp16(n_);
  rec.tariff = tariff_;
  if (counters_) {
    const bool same_source = prev && ((prev->flags & REC_DEMO) != 0) == demo;
    const bool contiguous = same_source && (prev->flags & REC_COUNTERS) &&
                            prev->ts + 60 == minute_ts && prev->imp_end <= imp_last_ &&
                            prev->exp_end <= exp_last_ && imp_last_ - prev->imp_end <= kMaxMinuteWh &&
                            exp_last_ - prev->exp_end <= kMaxMinuteWh;
    const uint32_t imp_start = contiguous ? prev->imp_end : imp_first_;
    const uint32_t exp_start = contiguous ? prev->exp_end : exp_first_;
    rec.imp_end = imp_last_;
    rec.exp_end = exp_last_;
    // A counter that goes backwards (meter swap, demo restart) counts as zero
    rec.imp_Wh = imp_last_ >= imp_start ? imp_last_ - imp_start : 0;
    rec.exp_Wh = exp_last_ >= exp_start ? exp_last_ - exp_start : 0;
    rec.flags |= REC_COUNTERS;
  } else {
    rec.imp_Wh = (uint32_t)((p_sum_ / n_) / 60);  // W * 1/60 h
    rec.flags |= REC_ESTIMATED;
  }
  rec.cost_u = costMicro(rec.imp_Wh, price_u);
  if (demo) rec.flags |= REC_DEMO;
  return rec;
}

void RecAgg::add(const Rec &r) {
  if (r.samples == 0) return;
  n_++;
  imp_sum_ += r.imp_Wh;
  exp_sum_ += r.exp_Wh;
  cost_sum_ += r.cost_u;
  p_sum_ += r.p_avg;
  if (r.p_max >= p_max_) { p_max_ = r.p_max; peak_ts_ = r.ts; }
  const uint16_t low = minutes_ ? r.p_avg : r.p_min;
  if (low < p_min_) p_min_ = low;
  for (int k = 0; k < 3; k++) {
    pl_sum_[k] += r.pl_avg[k];
    if (r.v_avg[k]) { v_sum_[k] += r.v_avg[k]; v_n_[k]++; }
    if (r.i_max[k] > i_max_[k]) i_max_[k] = r.i_max[k];
  }
  if (r.flags & REC_COUNTERS) {
    const uint32_t start = r.imp_end - r.imp_Wh, estart = r.exp_end - r.exp_Wh;
    if (counters_ && r.ts > last_ts_) {
      // Energy the meter counted while we were not recording (reboot, update)
      const uint32_t period = minutes_ ? 60 : 3600;
      const uint32_t off_s = r.ts > last_ts_ + period ? r.ts - (last_ts_ + period) : 0;
      const uint64_t max_wh = (uint64_t)off_s * kMaxGapW / 3600 + 50;
      if (start > imp_end_ && start - imp_end_ <= max_wh) gap_imp_ += start - imp_end_;
      if (estart > exp_end_ && estart - exp_end_ <= max_wh) gap_exp_ += estart - exp_end_;
    }
    counters_ = true;
    imp_end_ = r.imp_end;
    exp_end_ = r.exp_end;
  }
  last_ts_ = r.ts;
  tariff_ = r.tariff;
  flags_ |= r.flags;
}

void RecAgg::seed(const Rec &prev) {
  if (n_ || !(prev.flags & REC_COUNTERS)) return;
  counters_ = true;
  imp_end_ = prev.imp_end;
  exp_end_ = prev.exp_end;
  last_ts_ = prev.ts;
}

Rec RecAgg::finish(uint32_t ts, uint32_t price_u) const {
  Rec rec;
  memset(&rec, 0, sizeof(rec));
  rec.ts = ts;
  if (n_ == 0) return rec;
  rec.p_avg = clamp16(p_sum_ / n_);
  rec.p_max = p_max_;
  rec.p_min = p_min_;
  for (int k = 0; k < 3; k++) {
    rec.pl_avg[k] = clamp16(pl_sum_[k] / n_);
    rec.v_avg[k] = v_n_[k] ? clamp16(v_sum_[k] / v_n_[k]) : 0;
    rec.i_max[k] = i_max_[k];
  }
  rec.samples = clamp16(n_);
  rec.tariff = tariff_;
  rec.flags = flags_;
  uint64_t imp = imp_sum_ + gap_imp_, exp = exp_sum_ + gap_exp_;
  uint64_t cost = cost_sum_ + costMicro((uint32_t)gap_imp_, price_u);
  if (counters_) {
    rec.imp_end = imp_end_;
    rec.exp_end = exp_end_;
  }
  rec.imp_Wh = (uint32_t)imp;
  rec.exp_Wh = (uint32_t)exp;
  rec.cost_u = cost > UINT32_MAX ? UINT32_MAX : (uint32_t)cost;
  return rec;
}
