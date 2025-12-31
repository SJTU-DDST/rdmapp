#pragma once

#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include <infiniband/verbs.h>

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

void execute_callback(struct ibv_wc const &wc) noexcept;

/**
 * @brief Destroy a callback function.
 *
 * @param cb The callback function pointer.
 */
void destroy_callback(callback_ptr cb);

} // namespace executor_t

struct ThisThread {};
struct WorkerThread {};
template <typename T>
concept ExecutorTag =
    std::is_same_v<T, ThisThread> || std::is_same_v<T, WorkerThread>;
template <ExecutorTag Tag> class basic_executor;
template <typename T>
concept ExecutorType = std::is_same_v<T, basic_executor<ThisThread>> ||
                       std::is_same_v<T, basic_executor<WorkerThread>>;

namespace detail {
struct executor_impl;
};

template <ExecutorTag Tag> class basic_executor {
  std::unique_ptr<detail::executor_impl> impl_;

public:
  /**
   * @brief Construct a new executor object
   */
  basic_executor(int nr_workers = 4)
  requires std::same_as<Tag, WorkerThread>;

  basic_executor()
  requires std::same_as<Tag, ThisThread>;

  /**
   * @brief Process a completion entry.
   *
   * @param wc The completion entry to process.
   */
  void process_wc(std::span<struct ibv_wc> const wc) noexcept;

  /**
   * @brief Shutdown the executor.
   *
   */
  void shutdown();

  ~basic_executor();
};

using executor = basic_executor<ThisThread>;

} // namespace rdmapp
