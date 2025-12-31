#include "rdmapp/executor.h"

#include <concurrentqueue.h>
#include <spdlog/fmt/std.h>
#include <stop_token>
#include <thread>

#include "rdmapp/detail/logger.h"

namespace rdmapp {

namespace executor_t {
void execute_callback(struct ibv_wc const &wc) noexcept {
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("process_wc: {:#x}", wc.wr_id);
#endif
  auto cb = reinterpret_cast<callback_ptr>(wc.wr_id);
  (*cb)(wc);
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("process_wc: done: {:#x}", wc.wr_id);
#endif
  destroy_callback(cb);
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("process_wc: callback destroyed: {:#x}", wc.wr_id);
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
struct executor_impl {
  moodycamel::ConcurrentQueue<ibv_wc> work_chan_;
  std::vector<std::jthread> workers_;

  executor_impl(int workers) : work_chan_(4) {
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
    constexpr int batch_size = 4;
    std::array<ibv_wc, batch_size> wc_buffer;
    while (!token.stop_requested()) {
      if (int k = work_chan_.try_dequeue_bulk(wc_buffer.begin(), batch_size);
          k > 0) {
        for (int i = 0; i < k; i++)
          executor_t::execute_callback(wc_buffer[i]);
      }
    }
  }
};

} // namespace detail

template <ExecutorTag Tag>
basic_executor<Tag>::basic_executor()
requires std::same_as<Tag, ThisThread>
{}

template <ExecutorTag Tag>
basic_executor<Tag>::basic_executor(int nr_workers)
requires std::same_as<Tag, WorkerThread>
    : impl_(std::make_unique<detail::executor_impl>(nr_workers)) {}

template <ExecutorTag Tag>
void basic_executor<Tag>::process_wc(
    std::span<struct ibv_wc> const wc) noexcept {
  if constexpr (std::is_same_v<Tag, WorkerThread>) {
    impl_->work_chan_.enqueue_bulk(wc.data(), wc.size());
  } else if constexpr (std::is_same_v<Tag, ThisThread>) {
    for (auto const &w : wc) {
      executor_t::execute_callback(w);
    }
  } else {
    static_assert(0);
  }
}

template <ExecutorTag Tag> void basic_executor<Tag>::shutdown() {
  if (impl_)
    impl_->shutdown();
}

template <ExecutorTag Tag> basic_executor<Tag>::~basic_executor() {
  shutdown();
}

template class basic_executor<ThisThread>;
template class basic_executor<WorkerThread>;

} // namespace rdmapp
