#pragma once

#include <cstddef>

namespace ProgressFormat {

// Formats a 0-100 progress percentage into buf as "<value><decimals>%",
// e.g. formatPercent(buf, size, 42.5f, 1) -> "42.5%".
// decimalPlaces is clamped to [0, 2].
void formatPercent(char* buf, size_t bufSize, float percent, unsigned int decimalPlaces);

}  // namespace ProgressFormat
