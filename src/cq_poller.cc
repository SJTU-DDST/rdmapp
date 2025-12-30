#include "rdmapp/cq_poller.h"

#include <memory>
#include <spdlog/fmt/std.h>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include <infiniband/verbs.h>

#include "rdmapp/executor.h"

#include "rdmapp/detail/logger.h"

namespace rdmapp {

template <ExecutorType Executor>
basic_cq_poller<Executor>::basic_cq_poller(std::shared_ptr<cq> cq,
                                           std::unique_ptr<Executor> executor,
                                           size_t batch_size)
    : wc_vec_(batch_size), cq_(cq), executor_(std::move(executor)),
      poller_thread_(&basic_cq_poller::worker, this) {}

template <ExecutorType Executor>
basic_cq_poller<Executor>::~basic_cq_poller() {}

template <ExecutorType Executor>
void basic_cq_poller<Executor>::worker(std::stop_token token) {
  log::debug("cq_poller[thread={}]: polling cqe", std::this_thread::get_id());
  while (!token.stop_requested()) {
    try {
      size_t nr_wc = cq_->poll(wc_vec_);
      if (!nr_wc)
        continue;
      log::trace("polled cqe: nr_wc={} ", nr_wc);
      executor_->process_wc({wc_vec_.begin(), nr_wc});
    } catch (std::runtime_error &e) {
      log::error("cq_poller[thread={}: exception: {}",
                 std::this_thread::get_id(), e.what());
      return;
    }
  }
  log::debug("cq_poller[thread={}]: polling cqe exited",
             std::this_thread::get_id());
}

template class basic_cq_poller<basic_executor<ThisThread>>;
template class basic_cq_poller<basic_executor<WorkerThread>>;

} // namespace rdmapp
