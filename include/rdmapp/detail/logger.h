#pragma once

#include <memory>
#include <spdlog/spdlog.h>
#include <string_view>

#include "rdmapp/log.h"

namespace rdmapp::log::detail {
struct logger {
public:
  // 禁止拷贝和移动
  logger(const logger &) = delete;
  logger &operator=(const logger &) = delete;

  auto setup(level level = level::info, std::string_view log_path = "") -> void;

  static auto get_instance() -> logger &;

  auto get_logger() -> spdlog::logger &;

private:
  logger();
  ~logger() = default;

  std::unique_ptr<spdlog::logger> spdlogger_;
  std::mutex setup_mutex_;
};
} // namespace rdmapp::log::detail

namespace rdmapp::log {

template <typename... Args>
void trace(fmt::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().get_logger().trace(
      fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void debug(fmt::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().get_logger().debug(
      fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void info(fmt::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().get_logger().info(fmt,
                                                   std::forward<Args>(args)...);
}
template <typename... Args>
void warn(fmt::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().get_logger().warn(fmt,
                                                   std::forward<Args>(args)...);
}
template <typename... Args>
void error(fmt::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().get_logger().error(
      fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void critical(fmt::format_string<Args...> fmt, Args &&...args) {
  detail::logger::get_instance().get_logger().critical(
      fmt, std::forward<Args>(args)...);
}

} // namespace rdmapp::log
