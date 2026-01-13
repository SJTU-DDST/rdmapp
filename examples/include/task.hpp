#pragma once

#include "future.hpp"
#include <cassert>
#include <coroutine>
#include <exception>
#include <utility>

template <class T> struct value_returner {
  utils::SpinPromise<T> promise_;

  void return_value(T &&value) {
    promise_.set_value(std::forward<T>(value), false);
  }
};

template <> struct value_returner<void> {
  utils::SpinPromise<void> promise_;
  void return_void() {}
};

template <class T, class CoroutineHandle>
struct promise_base : public value_returner<T> {
  std::suspend_never initial_suspend() { return {}; }
  auto final_suspend() noexcept {
    struct awaiter {
      bool await_ready() noexcept { return false; }
      void await_resume() noexcept {}
      auto await_suspend(CoroutineHandle h) noexcept
          -> std::coroutine_handle<> {
        assert(h.done());
        h.promise().set_return_value();
        if (h.promise().continuation_) {
          return h.promise().continuation_;
        }
        return std::noop_coroutine();
      }
    };
    return awaiter{};
  }
  std::coroutine_handle<> continuation_;
};

template <class T> struct task {
  struct promise_type
      : public promise_base<T, std::coroutine_handle<promise_type>> {
    task<T> get_return_object() {
      return std::coroutine_handle<promise_type>::from_promise(*this);
    }
    void unhandled_exception() {
      this->promise_.set_exception(std::current_exception(), false);
    }
    promise_type() : future_(this->promise_.get_future()) {}
    auto &get_future() { return future_; }

    auto set_return_value() noexcept { this->promise_.notify(); }

    utils::SpinFuture<T> future_;
  };

  struct task_awaiter {
    std::coroutine_handle<promise_type> h_;
    task_awaiter(std::coroutine_handle<promise_type> h) : h_(h) {}
    bool await_ready() { return h_.done(); }
    void await_suspend(std::coroutine_handle<> suspended) {
      h_.promise().continuation_ = suspended;
    }
    auto await_resume() { return h_.promise().future_.get(); }
  };

  auto operator co_await() const noexcept { return task_awaiter{h_}; }

  using coroutine_handle_type = std::coroutine_handle<promise_type>;

  ~task() {
    if (!h_)
      return;
    get_future().wait();
    assert(h_.done());
    h_.destroy();
  }

  task(task &&other) : h_(std::exchange(other.h_, nullptr)) {}
  task(coroutine_handle_type h) : h_(h) {}
  coroutine_handle_type h_;
  operator coroutine_handle_type() const { return h_; }
  auto &get_future() const { return h_.promise().get_future(); }
};
