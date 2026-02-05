#include "rdmapp/cq_poller.h"

#include "rdmapp/completion_token.h"
#include "rdmapp/detail/logger.h"
#include "rdmapp/executor.h"
#include "rdmapp/qp.h"
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

#ifdef RDMAPP_ASIO_COROUTINE
static void process_wc_asio(std::span<struct ibv_wc const> wc_vec) noexcept {
  for (auto &wc : wc_vec) {
#ifdef RDMAPP_BUILD_DEBUG
    auto thread_id = std::this_thread::get_id();
    log::trace("process_wc[thread={}]: {:#x}", thread_id, wc.wr_id);
#endif
    auto cb = reinterpret_cast<executor_t::callback_ptr>(wc.wr_id);
    (*cb)(wc);
#ifdef RDMAPP_BUILD_DEBUG
    log::trace("process_wc[thread={}]: done: {:#x}", thread_id, wc.wr_id);
#endif
    executor_t::destroy_callback(cb);
#ifdef RDMAPP_BUILD_DEBUG
    log::trace("process_wc[thread={}]: callback destroyed: {:#x}", thread_id,
               wc.wr_id);
#endif
  }
}
#endif

basic_cq_poller<use_native_awaitable_t>::basic_cq_poller(
    std::shared_ptr<cq> cq, std::shared_ptr<scheduler> scheduler,
    size_t batch_size)
    : wc_vec_(batch_size), cq_(std::move(cq)),
      poller_thread_(std::bind_front(&basic_cq_poller::worker, this)),
      scheduler_(std::move(scheduler)) {}

basic_cq_poller<use_native_awaitable_t>::~basic_cq_poller() {}

void basic_cq_poller<use_native_awaitable_t>::worker(std::stop_token token) {
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

#ifdef RDMAPP_ASIO_COROUTINE
basic_cq_poller<use_asio_awaitable_t>::basic_cq_poller(std::shared_ptr<cq> cq,
                                                       size_t batch_size)
    : wc_vec_(batch_size), cq_(std::move(cq)),
      poller_thread_(std::bind_front(&basic_cq_poller::worker, this)) {}

basic_cq_poller<use_asio_awaitable_t>::~basic_cq_poller() {}

void basic_cq_poller<use_asio_awaitable_t>::worker(std::stop_token token) {
  log::debug("cq_poller[thread={}]: polling cqe", std::this_thread::get_id());
  while (!token.stop_requested()) {
    try {
      size_t nr_wc = cq_->poll(wc_vec_);
      if (!nr_wc)
        continue;
      log::trace("polled cqe: nr_wc={} ", nr_wc);
      process_wc_asio({wc_vec_.data(), nr_wc});
    } catch (std::runtime_error &e) {
      log::error("cq_poller[thread={}: exception: {}",
                 std::this_thread::get_id(), e.what());
      return;
    }
  }
  log::debug("cq_poller[thread={}]: polling cqe exited",
             std::this_thread::get_id());
}
#endif

} // namespace rdmapp
