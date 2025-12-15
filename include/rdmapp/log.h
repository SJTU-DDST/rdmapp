#pragma once

#include <string_view>

namespace rdmapp::log {

enum class level { trace = 0, debug, info, warn, error, critical, off };

/**
 * @brief configure global logger
 * @param level log level
 * @param log_path if not empty, write to console and file path
 */
void setup(level level, const std::string_view log_path = "");

} // namespace rdmapp::log
