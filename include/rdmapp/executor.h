#pragma once

#include <functional>
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
class executor {

public:
  using callback_fn = std::function<void(struct ibv_wc const &wc)>;
  using callback_ptr = callback_fn *;

  /**
   * @brief Construct a new executor object
   */
  executor();

  /**
   * @brief Process a completion entry.
   *
   * @param wc The completion entry to process.
   */
  void process_wc(struct ibv_wc const &wc) noexcept;

  /**
   * @brief Shutdown the executor.
   *
   */
  void shutdown();

  ~executor();

  /**
   * @brief Make a callback function that will be called when a completion entry
   * is processed. The callback function will be called in the executor's
   * thread. The lifetime of this pointer is controlled by the executor.
   * @tparam T The type of the callback function.
   * @param cb The callback function.
   * @return callback_ptr The callback function pointer.
   */
  template <class T> static callback_ptr make_callback(T &&cb) {
    auto ptr = new executor::callback_fn(std::forward<T>(cb));
#ifdef RDMAPP_BUILD_DEBUG
    log::trace("executor: make_callback: {}", fmt::ptr(ptr));
#endif
    return ptr;
  }

  /**
   * @brief Destroy a callback function.
   *
   * @param cb The callback function pointer.
   */
  static void destroy_callback(callback_ptr cb);
};

} // namespace rdmapp
