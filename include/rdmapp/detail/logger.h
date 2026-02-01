#pragma once

#include "rdmapp/log.h"
#include <atomic>
#include <cstdio>
#include <format>
#include <sstream>
#include <string>
#include <thread>

template <>
struct std::formatter<std::thread::id> : std::formatter<std::string> {
  auto format(std::thread::id id, format_context &ctx) const {
    std::ostringstream ss;
    ss << id;
    return std::formatter<std::string>::format(ss.str(), ctx);
  }
};

namespace rdmapp::log::detail {

class logger {
public:
  void set_level(level level) {
    level_.store(level, std::memory_order_relaxed);
  }

  template <typename... Args>
  void trace(std::format_string<Args...> fmt, Args &&...args) {
    log(level::trace, "TRACE", fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void debug(std::format_string<Args...> fmt, Args &&...args) {
    log(level::debug, "DEBUG", fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void info(std::format_string<Args...> fmt, Args &&...args) {
    log(level::info, "INFO ", fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void warn(std::format_string<Args...> fmt, Args &&...args) {
    log(level::warn, "WARN ", fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void error(std::format_string<Args...> fmt, Args &&...args) {
    log(level::error, "ERROR", fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  void critical(std::format_string<Args...> fmt, Args &&...args) {
    log(level::critical, "CRIT ", fmt, std::forward<Args>(args)...);
  }

  static logger &get_instance() {
    static logger instance;
    return instance;
  }

  void setup(level level = level::info, std::string_view log_path = "") {
    set_level(level);
    if (!log_path.empty()) {
      // For now, we don't implement file logging to keep it simple as per the
      // reference. If file logging is required, we can add it later.
    }
  }

private:
  template <typename... Args>
  void log(level level, const char *level_str, std::format_string<Args...> fmt,
           Args &&...args) {
    if (level >= level_.load(std::memory_order_relaxed)) {
      std::string s =
          std::format("[rdmapp] [{}] {}\n", level_str,
                      std::format(fmt, std::forward<Args>(args)...));
      std::printf("%s", s.c_str());
    }
  }

  std::atomic<level> level_{level::info};
};

} // namespace rdmapp::log::detail

namespace rdmapp::log {

namespace fmt {
template <typename T> const void *ptr(T &&p) {
  if constexpr (requires { p.get(); }) {
    return static_cast<const void *>(p.get());
  } else {
    return static_cast<const void *>(p);
  }
}
} // namespace fmt

} // namespace rdmapp::log

namespace rdmapp::log {

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().trace(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void debug(std::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().debug(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void info(std::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().info(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void warn(std::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().warn(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void error(std::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().error(fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void critical(std::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().critical(fmt, std::forward<Args>(args)...);
}

} // namespace rdmapp::log
