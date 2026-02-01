#include "rdmapp/cq_poller.h"

#include <memory>
#include <thread>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include <infiniband/verbs.h>

#include "rdmapp/completion_token.h"
#include "rdmapp/executor.h"

#include "rdmapp/detail/logger.h"

namespace rdmapp {

template <typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
basic_cq_poller<CompletionToken>::basic_cq_poller(std::shared_ptr<cq> cq,
                                                  size_t batch_size)
    : wc_vec_(batch_size), cq_(std::move(cq)),
      poller_thread_(std::bind_front(&basic_cq_poller::worker, this)) {}

template <typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
basic_cq_poller<CompletionToken>::~basic_cq_poller() {}

template <typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
void basic_cq_poller<CompletionToken>::worker(std::stop_token token) {
  log::debug("cq_poller[thread={}]: polling cqe", std::this_thread::get_id());
  while (!token.stop_requested()) {
    try {
      size_t nr_wc = cq_->poll(wc_vec_);
      if (!nr_wc)
        continue;
      log::trace("polled cqe: nr_wc={} ", nr_wc);
      basic_executor::process_wc<CompletionToken>({wc_vec_.begin(), nr_wc});
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
template class basic_cq_poller<use_asio_awaitable_t>;
#endif
template class basic_cq_poller<use_native_awaitable_t>;

} // namespace rdmapp
