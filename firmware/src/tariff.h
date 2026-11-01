// Electricity price with time-of-day discounts (supplier plans such as
// "night" or "day" discounts). Pure C++, unit-tested on the PC.
#pragma once
#include <stdint.h>

#define MAX_DISCOUNT_RULES 4

struct DiscountRule {
  uint8_t enabled = 0;
  uint8_t days = 0x7F;     // bit 0 = Sunday ... bit 6 = Saturday
  uint8_t start_hour = 0;  // 0..23
  uint8_t end_hour = 24;   // 1..24, exclusive. end <= start wraps past midnight (23 -> 7)
  uint16_t pct_x10 = 0;    // discount in 0.1% (150 = 15%)
};

struct TariffSettings {
  uint32_t price_u = 0;     // price per kWh incl. VAT, micro-ILS (0.6352 ILS = 635200)
  uint32_t fixed_ag = 0;    // fixed charge per month, agorot (shown in the forecast only)
  uint8_t bill_day = 1;     // first day of the billing month (1..28)
  DiscountRule rules[MAX_DISCOUNT_RULES];
};

// Does the rule cover this local weekday (0 = Sunday) and hour (0..23)?
bool ruleMatches(const DiscountRule &r, int wday, int hour);

// Discount in 0.1% at this local time: the largest matching rule
uint16_t discountAt(const TariffSettings &t, int wday, int hour);

// Price per kWh after the discount, micro-ILS
uint32_t priceAt(const TariffSettings &t, int wday, int hour);
