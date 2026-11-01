#include "tariff.h"

bool ruleMatches(const DiscountRule &r, int wday, int hour) {
  if (!r.enabled || r.pct_x10 == 0) return false;
  if (wday < 0 || wday > 6 || hour < 0 || hour > 23) return false;
  if (!(r.days & (1 << wday))) return false;
  const int s = r.start_hour, e = r.end_hour;
  if (s < e) return hour >= s && hour < e;  // e.g. 7 -> 17
  return hour >= s || hour < e;             // e.g. 23 -> 7, or 0 -> 0 = all day
}

uint16_t discountAt(const TariffSettings &t, int wday, int hour) {
  uint16_t best = 0;
  for (const DiscountRule &r : t.rules)
    if (ruleMatches(r, wday, hour) && r.pct_x10 > best) best = r.pct_x10;
  return best > 1000 ? 1000 : best;
}

uint32_t priceAt(const TariffSettings &t, int wday, int hour) {
  const uint16_t d = discountAt(t, wday, hour);
  return (uint32_t)(((uint64_t)t.price_u * (1000 - d) + 500) / 1000);
}
