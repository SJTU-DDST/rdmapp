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
    if (wc.status == IBV_WC_SUCCESS) {
      LOGT("wc success wr_id={:#x} opcode={} byte_len={} imm_flags={:#x}",
           wc.wr_id, static_cast<int>(wc.opcode), wc.byte_len, wc.wc_flags);
    } else {
      log::error("wc error wr_id={:#x} status={} opcode={} byte_len={} "
                 "vendor_err={:#x}",
                 wc.wr_id, static_cast<int>(wc.status),
                 static_cast<int>(wc.opcode), wc.byte_len, wc.vendor_err);
    }
    basic_qp::operation_state *state =
        reinterpret_cast<basic_qp::operation_state *>(wc.wr_id);
    state->set_from_wc(wc);
    scheduler.schedule(state->coro_handle);
  }
}

cq_poller::cq_poller(std::shared_ptr<cq> cq,
                     std::shared_ptr<scheduler> scheduler, size_t batch_size)
    : wc_vec_(batch_size), cq_(std::move(cq)), scheduler_(std::move(scheduler)),
      poller_thread_(std::bind_front(&cq_poller::worker, this)) {}

cq_poller::~cq_poller() {}

void cq_poller::worker(std::stop_token token) {
  log::debug("cq_poller[thread={}]: polling cqe", std::this_thread::get_id());
  while (!token.stop_requested()) {
    try {
      size_t nr_wc = cq_->poll(wc_vec_);
      if (!nr_wc) {
        continue;
      }
      log::debug("polled cqe: nr_wc={}", nr_wc);
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
