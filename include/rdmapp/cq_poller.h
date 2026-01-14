#pragma once

#include <memory>
#include <thread>

#include <infiniband/verbs.h>

#include "rdmapp/completion_token.h"
#include "rdmapp/cq.h"
#include "rdmapp/executor.h"

namespace rdmapp {

/**
 * @brief This class is used to poll a completion queue.
 *
 */

template <ExecutionThread Thread, typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
class basic_cq_poller {
  std::vector<struct ibv_wc> wc_vec_;
  std::shared_ptr<cq> cq_;
  std::shared_ptr<basic_executor<Thread>> executor_;

  std::jthread poller_thread_;
  void worker(std::stop_token token);

public:
  /**
   * @brief Construct a new cq poller object.
   *
   * @param cq The completion queue to poll.
   * @param executor The executor for polled wc callback execution
   * @param batch_size The number of completion entries to poll at a time.
   */
  basic_cq_poller(std::shared_ptr<cq> cq,
                  std::shared_ptr<basic_executor<Thread>> executor =
                      std::make_shared<basic_executor<Thread>>(),
                  size_t batch_size = 16);

  ~basic_cq_poller();
};

using cq_poller = basic_cq_poller<executor_t::ThisThread, use_asio_awaitable_t>;

using native_cq_poller =
    basic_cq_poller<executor_t::ThisThread, use_native_awaitable_t>;

} // namespace rdmapp
