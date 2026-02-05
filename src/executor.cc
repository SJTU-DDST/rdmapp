#include "rdmapp/executor.h"

namespace rdmapp::executor_t {

void destroy_callback(callback_ptr cb) noexcept {
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("executor: delete_callback: {}", log::fmt::ptr(cb));
#endif
  delete cb;
}

} // namespace rdmapp::executor_t
