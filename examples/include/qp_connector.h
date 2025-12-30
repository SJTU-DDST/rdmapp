

#pragma once

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <memory>

#include <rdmapp/cq.h>
#include <rdmapp/pd.h>
#include <rdmapp/qp.h>

#include "rdmapp/detail/noncopyable.h"

namespace rdmapp {

/**
 * @brief This class is used to actively connect to a remote endpoint and
 * establish a Queue Pair.
 *
 */
class qp_connector : public noncopyable {
  std::shared_ptr<pd> pd_;
  std::shared_ptr<cq> recv_cq_;
  std::shared_ptr<cq> send_cq_;
  std::shared_ptr<srq> srq_;
  std::string hostname_;
  uint16_t port_;

  asio::awaitable<std::shared_ptr<rdmapp::queue_pair<>>>
  from_socket(asio::ip::tcp::socket socket);

public:
  /**
   * @brief Construct a new connector object.
   *
   * @param hostname The hostname to connect to.
   * @param port The port to connect to.
   * @param pd The rdma protection domain.
   * @param recv_cq The recv completion queue to use for new Queue Pairs.
   * @param send_cq The send completion queue to use for new Queue Pairs.
   * @param srq (Optional) The shared receive queue to use for new Queue
   * Pairs.
   */
  qp_connector(std::string const &hostname, uint16_t port,
               std::shared_ptr<pd> pd, std::shared_ptr<cq> recv_cq,
               std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq = nullptr);

  /**
   * @brief Construct a new connector object.
   *
   * @param hostname The hostname to connect to.
   * @param port The port to connect to.
   * @param pd The rdma protection domain.
   * @param recv_cq The send/recv completion queue to use for new Queue Pairs.
   * @param srq (Optional) The shared receive queue to use for new Queue
   * Pairs.
   */
  qp_connector(std::string const &hostname, uint16_t port,
               std::shared_ptr<pd> pd, std::shared_ptr<cq> cq,
               std::shared_ptr<srq> srq = nullptr);

  /**
   * @brief This function is used to connect to a remote endpoint and
   * establish a Queue Pair.
   *
   * @return asio::awaitable<std::shared_ptr<qp>>
   */
  asio::awaitable<std::shared_ptr<qp>> connect();
};

} // namespace rdmapp
