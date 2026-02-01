#pragma once

#include "rdmapp/completion_token.h"
#include <functional>
#include <infiniband/verbs.h>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

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
  auto ptr = new (std::nothrow) callback_fn(std::forward<T>(cb));
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("executor: make_callback: {}", log::fmt::ptr(ptr));
#endif
  if (!ptr) [[unlikely]] {
#ifdef RDMAPP_BUILD_DEBUG
    log::error("executor: out of memory for callback_ptr");
#endif
    throw std::runtime_error("executor: out of memory for callback");
  }
  return ptr;
}

/**
 * @brief Destroy a callback function.
 *
 * @param cb The callback function pointer.
 */
void destroy_callback(callback_ptr cb) noexcept;

} // namespace executor_t

class basic_executor {

public:
  /**
   * @brief Construct a new executor object
   */
  basic_executor() noexcept;

  /**
   * @brief Process a completion entry.
   *
   * @param wc The completion entry to process.
   */
  template <typename CompletionToken>
  requires ValidCompletionToken<CompletionToken>
  static void process_wc(std::span<struct ibv_wc> const wc) noexcept;

  ~basic_executor() noexcept;
};

using executor = basic_executor;

} // namespace rdmapp
