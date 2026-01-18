#pragma once

#include <memory>
#include <thread>

#include <infiniband/verbs.h>

#include "rdmapp/completion_token.h"
#include "rdmapp/cq.h"

namespace rdmapp {

/**
 * @brief This class is used to poll a completion queue.
 *
 */

template <typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
class basic_cq_poller {
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

using cq_poller = basic_cq_poller<use_asio_awaitable_t>;

using native_cq_poller = basic_cq_poller<use_native_awaitable_t>;

} // namespace rdmapp
