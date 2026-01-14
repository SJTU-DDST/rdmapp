#include "rdmapp/executor.h"

#include <concurrentqueue.h>
#include <spdlog/fmt/std.h>
#include <stop_token>
#include <thread>

#include "rdmapp/completion_token.h"

#include "rdmapp/detail/logger.h"

namespace rdmapp {

namespace executor_t {

template <typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
void execute_callback(struct ibv_wc const &wc) noexcept;

template <>
void execute_callback<use_native_awaitable_t>(
    struct ibv_wc const &wc) noexcept {
  (void)wc;
}

template <>
void execute_callback<use_asio_awaitable_t>(struct ibv_wc const &wc) noexcept {
#ifdef RDMAPP_BUILD_DEBUG
  auto thread_id = std::this_thread::get_id();
  log::trace("process_wc[thread={}]: {:#x}", thread_id, wc.wr_id);
#endif
  auto cb = reinterpret_cast<callback_ptr>(wc.wr_id);
  (*cb)(wc);
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("process_wc[thread={}]: done: {:#x}", thread_id, wc.wr_id);
#endif
  destroy_callback(cb);
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("process_wc[thread={}]: callback destroyed: {:#x}", thread_id,
             wc.wr_id);
#endif
}

void destroy_callback(callback_ptr cb) {
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("executor: delete_callback: {}", fmt::ptr(cb));
#endif
  delete cb;
}
} // namespace executor_t

namespace detail {

using wc_processor_t = void (*)(struct ibv_wc const &);
struct work_item {
  struct ibv_wc wc;
  wc_processor_t processor;
};
struct executor_impl {
  moodycamel::ConcurrentQueue<work_item> work_chan_;
  std::vector<std::jthread> workers_;

  executor_impl(int workers) : work_chan_(workers * 128) {
    for (int i = 0; i < workers; i++) {
      workers_.emplace_back(&executor_impl::worker_fn, this);
    }
  }

  void shutdown() {
    for (auto &w : workers_) {
      w.request_stop();
    }
  }

  void worker_fn(std::stop_token token) {
    log::debug("[executor] worker_fn: spawn at thread_id={}",
               std::this_thread::get_id());
    while (!token.stop_requested()) {
      work_item it;
      if (work_chan_.try_dequeue(it))
        it.processor(it.wc);
    }
  }
};

} // namespace detail

template <ExecutionThread Thread>
basic_executor<Thread>::basic_executor()
requires std::same_as<Thread, executor_t::ThisThread>
{}

template <ExecutionThread Thread>
basic_executor<Thread>::basic_executor(int nr_workers)
requires std::same_as<Thread, executor_t::WorkerThread>
    : impl_(std::make_unique<detail::executor_impl>(nr_workers)) {}

template <ExecutionThread Thread>
template <typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
void basic_executor<Thread>::process_wc(
    std::span<struct ibv_wc> const wc) noexcept {
  if constexpr (std::is_same_v<Thread, executor_t::WorkerThread>) {
    for (auto const &w : wc) {
      bool ok = impl_->work_chan_.enqueue({
          w,
          executor_t::execute_callback<CompletionToken>,
      });
      if (!ok) [[unlikely]] {
        log::error("executor: failed to process wc, queue is not empty");
      }
    }
  } else if constexpr (std::is_same_v<Thread, executor_t::ThisThread>) {
    for (auto const &w : wc) {
      executor_t::execute_callback<CompletionToken>(w);
    }
  } else {
    static_assert(0);
  }
}

template <ExecutionThread Thread> void basic_executor<Thread>::shutdown() {
  if (impl_)
    impl_->shutdown();
}

template <ExecutionThread Thread> basic_executor<Thread>::~basic_executor() {
  shutdown();
}

template void
basic_executor<executor_t::ThisThread>::process_wc<use_native_awaitable_t>(
    std::span<struct ibv_wc> const) noexcept;

template void
basic_executor<executor_t::ThisThread>::process_wc<use_asio_awaitable_t>(
    std::span<struct ibv_wc> const) noexcept;

template void
basic_executor<executor_t::WorkerThread>::process_wc<use_native_awaitable_t>(
    std::span<struct ibv_wc> const) noexcept;

template void
basic_executor<executor_t::WorkerThread>::process_wc<use_asio_awaitable_t>(
    std::span<struct ibv_wc> const) noexcept;

template class basic_executor<executor_t::ThisThread>;
template class basic_executor<executor_t::WorkerThread>;

} // namespace rdmapp
