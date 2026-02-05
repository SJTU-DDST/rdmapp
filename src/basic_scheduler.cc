#include "rdmapp/scheduler.h"
#include <concurrentqueue.h>
#include <coroutine>
#include <stop_token>

namespace rdmapp {

static auto cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  __builtin_ia32_pause();
#endif
}

struct basic_scheduler::Impl {

  Impl(int capacity = 1024 * 512) noexcept : tasks_queue_(capacity) {}

  auto work_fn(std::stop_token token) -> void {
    while (!token.stop_requested()) {
      std::coroutine_handle<> h;
      if (!tasks_queue_.try_dequeue(h)) {
        cpu_relax();
        continue;
      }
      h.resume();
    }
  }

  auto run() -> void { this->work_fn(stop_source_.get_token()); }

  auto schedule(std::coroutine_handle<> h) noexcept -> void {
    tasks_queue_.enqueue(h);
  }

  auto schedule(std::span<std::coroutine_handle<>> h) noexcept -> void {
    tasks_queue_.enqueue_bulk(h.begin(), h.size());
  }

  auto stop() noexcept -> void { stop_source_.request_stop(); }

private:
  moodycamel::ConcurrentQueue<std::coroutine_handle<>> tasks_queue_;
  std::stop_source stop_source_;
};

basic_scheduler::basic_scheduler()
    : impl_(std::make_unique<basic_scheduler::Impl>()) {}

auto basic_scheduler::run() -> void { impl_->run(); }

auto basic_scheduler::stop() -> void { impl_->stop(); }

auto basic_scheduler::schedule(std::coroutine_handle<> h) noexcept -> void {
  impl_->schedule(h);
}

auto basic_scheduler::schedule(std::span<std::coroutine_handle<>> h) noexcept
    -> void {
  impl_->schedule(h);
}

basic_scheduler::~basic_scheduler() = default;

} // namespace rdmapp
