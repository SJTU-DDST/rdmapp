#pragma once

#include <concepts>
#include <memory>
#include <thread>

#include <infiniband/verbs.h>

#include "rdmapp/cq.h"
#include "rdmapp/executor.h"

namespace rdmapp {

/**
 * @brief This class is used to poll a completion queue.
 *
 */

template <executor_concept Executor> class basic_cq_poller {
  std::vector<struct ibv_wc> wc_vec_;
  std::shared_ptr<cq> cq_;
  std::unique_ptr<Executor> executor_;

  std::jthread poller_thread_;
  void worker(std::stop_token token);

public:
  /**
   * @brief Construct a new cq poller object.
   *
   * @param cq The completion queue to poll.
   * @param batch_size The number of completion entries to poll at a time.
   */
  basic_cq_poller(
      std::shared_ptr<cq> cq,
      std::unique_ptr<Executor> executor = std::make_unique<Executor>(),
      size_t batch_size = 16);

  ~basic_cq_poller();
};

using cq_poller = basic_cq_poller<executor>;

template <typename T>
concept cq_poller_concept =
    std::same_as<T, basic_cq_poller<basic_executor<executor_t::ThisThread>>> ||
    std::same_as<T, basic_cq_poller<basic_executor<executor_t::WorkerThread>>>;

static_assert(cq_poller_concept<cq_poller>);

} // namespace rdmapp
