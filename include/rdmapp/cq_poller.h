#pragma once

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
class cq_poller {
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
  cq_poller(std::shared_ptr<cq> cq, std::shared_ptr<scheduler> scheduler,
            size_t batch_size = 16);

  ~cq_poller();
};

using native_cq_poller = cq_poller;

} // namespace rdmapp
