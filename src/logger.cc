
#include "rdmapp/detail/logger.h"

#include "rdmapp/log.h"

namespace rdmapp::log {
void setup(level level, const std::string_view log_path) {
  auto &logger_ = detail::logger::get_instance();
  logger_.setup(level, log_path);
}

} // namespace rdmapp::log
