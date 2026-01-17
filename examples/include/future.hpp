#pragma once

#include <atomic>
#include <concepts>
#include <memory>
#include <optional>
#include <type_traits>

namespace utils {

template <typename T> struct alignas(64) SharedState {
  std::atomic_flag ready{};
  std::exception_ptr exception{};

  struct Empty {};
  [[no_unique_address]] std::conditional_t<std::is_void_v<T>, Empty,
                                           std::optional<T>> value;

  SharedState() = default;
  SharedState(const SharedState &) = delete;
};

template <typename T> class SpinFuture;
template <typename T> class SpinPromise;

template <> class SpinPromise<void> {
public:
  SpinPromise() : state(std::make_shared<SharedState<void>>()) {}

  SpinFuture<void> get_future() noexcept;

  auto set_exception(std::exception_ptr e, bool notify = true) noexcept
      -> void {
    state->exception = std::move(e);
    if (notify)
      this->notify();
  }

  void set_value(bool notify = true) {
    if (notify)
      this->notify();
  }

  void notify() noexcept {
    state->ready.test_and_set(std::memory_order_release);
  }

private:
  std::shared_ptr<SharedState<void>> state;
};

template <typename T> class SpinPromise {
public:
  SpinPromise() : state(std::make_shared<SharedState<T>>()) {}

  SpinFuture<T> get_future() noexcept;

  void notify() noexcept {
    state->ready.test_and_set(std::memory_order_release);
  }

  auto set_exception(std::exception_ptr e, bool notify = true) noexcept
      -> void {
    state->exception = std::move(e);
    if (notify)
      this->notify();
  }

  void set_value(T &&val, bool notify = true) {
    state->value = std::move(val);
    if (notify)
      this->notify();
  }

  void set_value(const T &val, bool notify = true) {
    state->value = val;
    if (notify)
      state->ready.test_and_set(std::memory_order_release);
  }

private:
  std::shared_ptr<SharedState<T>> state;
};

template <typename T> class SpinFuture {
public:
  explicit SpinFuture(std::shared_ptr<SharedState<T>> s)
      : state(std::move(s)) {}

  auto get() {
    wait();
    if (state->exception) {
      std::rethrow_exception(state->exception);
    }
    if constexpr (std::same_as<T, void>) {
      return;
    } else {
      return std::move(*(state->value));
    }
  }

  auto wait() noexcept -> void {
    while (state->ready.test(std::memory_order_acquire) == false) {
#if defined(__x86_64__) || defined(_M_X64)
      __builtin_ia32_pause();
#endif
    }
  }

  auto is_ready() const noexcept -> bool {
    return state->ready.test(std::memory_order_acquire);
  }

private:
  std::shared_ptr<SharedState<T>> state;
};

template <typename T> SpinFuture<T> SpinPromise<T>::get_future() noexcept {
  return SpinFuture<T>(state);
}

inline SpinFuture<void> SpinPromise<void>::get_future() noexcept {
  return SpinFuture<void>(state);
}
} // namespace utils
