#pragma once

#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace examples {

struct payload_size_args {
  std::size_t payload_size;
  std::vector<std::string_view> positional;
};

inline std::size_t parse_payload_size(std::string_view value) {
  if (value.empty()) {
    throw std::invalid_argument("payload size is empty");
  }

  std::size_t multiplier = 1;
  char suffix = value.back();
  if (suffix == 'k' || suffix == 'K') {
    multiplier = 1024;
    value.remove_suffix(1);
  } else if (suffix == 'm' || suffix == 'M') {
    multiplier = 1024 * 1024;
    value.remove_suffix(1);
  } else if (suffix == 'g' || suffix == 'G') {
    multiplier = 1024 * 1024 * 1024;
    value.remove_suffix(1);
  }

  if (value.empty()) {
    throw std::invalid_argument("payload size has no numeric part");
  }

  std::size_t size = 0;
  auto *begin = value.data();
  auto *end = value.data() + value.size();
  auto [ptr, ec] = std::from_chars(begin, end, size);
  if (ec != std::errc() || ptr != end) {
    throw std::invalid_argument("invalid payload size: " + std::string(value));
  }
  if (size == 0) {
    throw std::invalid_argument("payload size must be greater than zero");
  }
  if (size > static_cast<std::size_t>(-1) / multiplier) {
    throw std::overflow_error("payload size is too large");
  }

  return size * multiplier;
}

inline payload_size_args parse_payload_size_args(int argc, char *argv[],
                                                 std::size_t default_size) {
  payload_size_args result{default_size, {}};
  result.positional.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    constexpr std::string_view kOption = "--payload-size";
    constexpr std::string_view kShortOption = "-s";

    if (arg == kOption || arg == kShortOption) {
      if (i + 1 >= argc) {
        throw std::invalid_argument("missing value after " + std::string(arg));
      }
      result.payload_size = parse_payload_size(argv[++i]);
      continue;
    }

    constexpr std::string_view kOptionWithEquals = "--payload-size=";
    if (arg.starts_with(kOptionWithEquals)) {
      result.payload_size = parse_payload_size(arg.substr(kOptionWithEquals.size()));
      continue;
    }

    result.positional.push_back(arg);
  }

  return result;
}

inline std::string payload_size_usage() {
  return " [--payload-size <bytes|K|M|G>]";
}

} // namespace examples
