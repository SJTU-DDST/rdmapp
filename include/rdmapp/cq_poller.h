#pragma once

#include "rdmapp/completion_token.h"
#include "rdmapp/cq.h"
#include "rdmapp/scheduler.h"
#include <infiniband/verbs.h>
#include <memory>
#include <thread>
#include <vector>

namespace rdmapp {

/**
 * @brief This class is used to poll a completion queue.
 *
 */
template <typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
class basic_cq_poller;

template <> class basic_cq_poller<use_native_awaitable_t> {
  std::vector<struct ibv_wc> wc_vec_;
  std::shared_ptr<cq> cq_;

  std::jthread poller_thread_;
  std::shared_ptr<scheduler> scheduler_;
  void worker(std::stop_token token);

public:
  /**
   * @brief Construct a new cq poller object.
   *
   * @param cq The completion queue to poll.
   * @param scheduler The scheduler to use.
   * @param batch_size The number of completion entries to poll at a time.
   */
  basic_cq_poller(std::shared_ptr<cq> cq, std::shared_ptr<scheduler> scheduler,
                  size_t batch_size = 16);

  ~basic_cq_poller();
};

#ifdef RDMAPP_ASIO_COROUTINE
template <> class basic_cq_poller<use_asio_awaitable_t> {
  std::vector<struct ibv_wc> wc_vec_;
  std::shared_ptr<cq> cq_;

  std::jthread poller_thread_;
  void worker(std::stop_token token);

public:
  /**
   * @brief Construct a new cq poller object.
   *
   * @param cq The completion queue to poll.
   * @param batch_size The number of completion entries to poll at a time.
   */
  basic_cq_poller(std::shared_ptr<cq> cq, size_t batch_size = 16);

  ~basic_cq_poller();
};
#endif

#ifdef RDMAPP_ASIO_COROUTINE
using cq_poller = basic_cq_poller<use_asio_awaitable_t>;
#else
using cq_poller = basic_cq_poller<use_native_awaitable_t>;
#endif

using native_cq_poller = basic_cq_poller<use_native_awaitable_t>;

} // namespace rdmapp
