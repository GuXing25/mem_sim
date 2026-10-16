#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hbm_sim {

inline std::uint64_t rounded_bytes_from_bits(std::uint64_t bits) {
  return (bits + 7) / 8;
}

inline std::uint64_t checked_multiply_u64(
    std::uint64_t lhs, std::uint64_t rhs, std::string_view context,
    std::string_view overflow_suffix = " exceeds uint64 range") {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    throw std::overflow_error(std::string(context) + std::string(overflow_suffix));
  }
  return lhs * rhs;
}

} // namespace hbm_sim
