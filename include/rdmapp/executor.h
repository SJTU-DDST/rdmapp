#pragma once

#include <functional>
#include <utility>

#include <infiniband/verbs.h>

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
  void process_wc(struct ibv_wc const &wc);

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
    return new executor::callback_fn(std::forward<T>(cb));
  }

  /**
   * @brief Destroy a callback function.
   *
   * @param cb The callback function pointer.
   */
  static void destroy_callback(callback_ptr cb);
};

} // namespace rdmapp
