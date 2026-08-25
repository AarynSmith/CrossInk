#include "ProgressFormat.h"

#include <algorithm>
#include <cstdio>

namespace ProgressFormat {

void formatPercent(char* buf, size_t bufSize, const float percent, const unsigned int decimalPlaces) {
  const int precision = std::clamp<int>(static_cast<int>(decimalPlaces), 0, 2);
  snprintf(buf, bufSize, "%.*f%%", precision, static_cast<double>(percent));
}

}  // namespace ProgressFormat
