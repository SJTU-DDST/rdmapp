
#include "rdmapp/detail/logger.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "rdmapp/log.h"

namespace rdmapp::log {
void setup(level level, const std::string_view log_path) {
  auto &logger_ = detail::logger::get_instance();
  logger_.setup(level, log_path);
}

} // namespace rdmapp::log

namespace rdmapp::log::detail {

logger::logger() { setup(); }

auto logger::get_logger() -> spdlog::logger & { return *spdlogger_; }

auto logger::get_instance() -> logger & {
  static logger l;
  return l;
}

static auto to_spdlog_level(level l) {
  switch (l) {
  case level::trace:
    return spdlog::level::trace;
  case level::debug:
    return spdlog::level::debug;
  case level::info:
    return spdlog::level::info;
  case level::warn:
    return spdlog::level::warn;
  case level::error:
    return spdlog::level::err;
  case level::critical:
    return spdlog::level::critical;
  case level::off:
    return spdlog::level::off;
  }
  return spdlog::level::info;
}

auto logger::setup(level level, std::string_view log_path) -> void {
  std::lock_guard<std::mutex> lock(setup_mutex_);

  try {
    std::vector<spdlog::sink_ptr> sinks;

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::trace); // sink 尽量放开
    sinks.push_back(console_sink);

    if (!log_path.empty()) {
      auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
          std::string{log_path}, true);
      file_sink->set_level(spdlog::level::trace); // sink 尽量放开
      sinks.push_back(file_sink);
    }

    auto new_logger =
        std::make_unique<spdlog::logger>("rdmapp", sinks.begin(), sinks.end());
    new_logger->set_level(to_spdlog_level(level)); // 主开关在这里
    new_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    new_logger->flush_on(spdlog::level::info);

    spdlogger_ = std::move(new_logger);

  } catch (const spdlog::spdlog_ex &ex) {
    if (spdlogger_) {
      spdlogger_->error(
          "failed to setup new logger: {}. using previous logger.", ex.what());
    } else {
      spdlog::error("failed to setup new logger: {}. using default logger.",
                    ex.what());
    }
  }
}

} // namespace rdmapp::log::detail
