#include "rdmapp/executor.h"

#include "rdmapp/detail/logger.h"

namespace rdmapp {
executor::executor() {}

void executor::process_wc(struct ibv_wc const &wc) {
  log::trace("process_wc: {:#x}", wc.wr_id);
  auto cb = reinterpret_cast<callback_ptr>(wc.wr_id);
  // NOTEL wc should not throw exception
  (*cb)(wc);
  log::trace("process_wc: done: {:#x}", wc.wr_id);
  destroy_callback(cb);
  log::trace("process_wc: callback destroyed: {:#x}", wc.wr_id);
}

void executor::shutdown() {}

void executor::destroy_callback(callback_ptr cb) {
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("executor: delete_callback: {}", fmt::ptr(cb));
#endif
  delete cb;
}

executor::~executor() { shutdown(); }

} // namespace rdmapp
