#include "rdmapp/cq_poller.h"

#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include <infiniband/verbs.h>

#include "rdmapp/executor.h"

#include "rdmapp/detail/logger.h"

namespace rdmapp {

cq_poller::cq_poller(std::shared_ptr<cq> cq, size_t batch_size)
    : cq_(cq), wc_vec_(batch_size), poller_thread_(&cq_poller::worker, this) {}

cq_poller::~cq_poller() {}

void cq_poller::worker(std::stop_token token) {
  log::debug("cq_poller: polling cqe");
  while (!token.stop_requested()) {
    try {
      auto nr_wc = cq_->poll(wc_vec_);
      for (size_t i = 0; i < nr_wc; ++i) {
        auto &wc = wc_vec_[i];
        log::trace("polled cqe wr_id={:#x} status={}", wc.wr_id,
                   static_cast<int>(wc.status));
        executor_.process_wc(wc);
      }
    } catch (std::runtime_error &e) {
      log::error("{}", e.what());
      return;
    }
  }
}

} // namespace rdmapp
