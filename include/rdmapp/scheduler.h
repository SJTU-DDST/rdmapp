#pragma once

#include <coroutine>
#include <memory>
#include <span>

namespace rdmapp {

struct scheduler {

  virtual ~scheduler();

  virtual auto run() -> void = 0;

  virtual auto schedule(std::coroutine_handle<> h) noexcept -> void = 0;

  virtual auto schedule(std::span<std::coroutine_handle<>> h) noexcept
      -> void = 0;

  virtual auto stop() -> void = 0;
};

struct basic_scheduler : scheduler {

  basic_scheduler();

  ~basic_scheduler() override;

  auto run() -> void override;

  auto schedule(std::coroutine_handle<> h) noexcept -> void override;

  auto schedule(std::span<std::coroutine_handle<>> h) noexcept -> void override;

  auto stop() -> void override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace rdmapp
