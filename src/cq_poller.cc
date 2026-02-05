#include "rdmapp/cq_poller.h"

#include "rdmapp/detail/logger.h"
#include "rdmapp/qp.h"
#include <functional>
#include <infiniband/verbs.h>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace rdmapp {

static void process_wc_native(scheduler &scheduler,
                              std::span<struct ibv_wc const> wc_vec) noexcept {
  for (auto &wc : wc_vec) {
    basic_qp::operation_state *state =
        reinterpret_cast<basic_qp::operation_state *>(wc.wr_id);
    state->set_from_wc(wc);
    scheduler.schedule(state->coro_handle);
  }
}

cq_poller::cq_poller(std::shared_ptr<cq> cq,
                     std::shared_ptr<scheduler> scheduler, size_t batch_size)
    : wc_vec_(batch_size), cq_(std::move(cq)),
      poller_thread_(std::bind_front(&cq_poller::worker, this)),
      scheduler_(std::move(scheduler)) {}

cq_poller::~cq_poller() {}

void cq_poller::worker(std::stop_token token) {
  log::debug("cq_poller[thread={}]: polling cqe", std::this_thread::get_id());
  while (!token.stop_requested()) {
    try {
      size_t nr_wc = cq_->poll(wc_vec_);
      if (!nr_wc)
        continue;
      log::trace("polled cqe: nr_wc={} ", nr_wc);
      process_wc_native(*scheduler_, {wc_vec_.data(), nr_wc});
    } catch (std::runtime_error &e) {
      log::error("cq_poller[thread={}: exception: {}",
                 std::this_thread::get_id(), e.what());
      return;
    }
  }
  log::debug("cq_poller[thread={}]: polling cqe exited",
             std::this_thread::get_id());
}

} // namespace rdmapp
