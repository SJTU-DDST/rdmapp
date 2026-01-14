#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <span>
#include <utility>

#include <infiniband/verbs.h>

#include "rdmapp/completion_token.h"

#ifdef RDMAPP_BUILD_DEBUG
#include "rdmapp/detail/logger.h"
#endif

namespace rdmapp {

/**
 * @brief This class is used to execute callbacks of completion entries.
 *
 */

namespace executor_t {
using callback_fn = std::function<void(struct ibv_wc const &wc)>;
using callback_ptr = callback_fn *;
/**
 * @brief Make a callback function that will be called when a completion entry
 * is processed. The callback function will be called in the executor's
 * thread. The lifetime of this pointer is controlled by the executor.
 * @tparam T The type of the callback function.
 * @param cb The callback function.
 * @return callback_ptr The callback function pointer.
 */
template <class T> static callback_ptr make_callback(T &&cb) {
  auto ptr = new callback_fn(std::forward<T>(cb));
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("executor: make_callback: {}", fmt::ptr(ptr));
#endif
  return ptr;
}

void execute_callback_fn(struct ibv_wc const &wc) noexcept;

/**
 * @brief Destroy a callback function.
 *
 * @param cb The callback function pointer.
 */
void destroy_callback(callback_ptr cb);

struct ThisThread {};
struct WorkerThread {};

} // namespace executor_t

template <typename T>
concept ExecutionThread = std::same_as<T, executor_t::ThisThread> ||
                          std::same_as<T, executor_t::WorkerThread>;

namespace detail {
struct executor_impl;
};

template <ExecutionThread Thread> class basic_executor {
  std::unique_ptr<detail::executor_impl> impl_;

public:
  /**
   * @brief Construct a new executor object with worker thread number
   */

  basic_executor(int nr_workers = 2)
  requires std::same_as<Thread, executor_t::WorkerThread>;

  /**
   * @brief Construct a new executor object
   */
  basic_executor()
  requires std::same_as<Thread, executor_t::ThisThread>;

  /**
   * @brief Process a completion entry.
   *
   * @param wc The completion entry to process.
   */

  template <typename CompletionToken>
  requires ValidCompletionToken<CompletionToken>
  void process_wc(std::span<struct ibv_wc> const wc) noexcept;

  /**
   * @brief Shutdown the executor.
   *
   */
  void shutdown();

  ~basic_executor();
};

using executor = basic_executor<executor_t::ThisThread>;

} // namespace rdmapp
