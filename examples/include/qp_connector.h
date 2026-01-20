

#pragma once

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <list>
#include <memory>

#include <rdmapp/cq.h>
#include <rdmapp/cq_poller.h>
#include <rdmapp/detail/noncopyable.h>
#include <rdmapp/pd.h>
#include <rdmapp/qp.h>

namespace rdmapp {

/**
 * @brief This class is used to actively connect to a remote endpoint and
 * establish a Queue Pair.
 *
 */
template <typename cq_poller_t> class basic_qp_connector : public noncopyable {
  std::shared_ptr<pd> pd_;
  std::shared_ptr<srq> srq_;
  std::string hostname_;
  uint16_t port_;
  std::list<cq_poller_t> pollers_;

  std::shared_ptr<rdmapp::cq> alloc_cq();

  asio::awaitable<std::shared_ptr<rdmapp::qp>>
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
  basic_qp_connector(std::string const &hostname, uint16_t port,
                     std::shared_ptr<pd> pd,
                     std::shared_ptr<srq> srq = nullptr);

  /**
   * @brief This function is used to connect to a remote endpoint and
   * establish a Queue Pair.
   *
   * @return asio::awaitable<std::shared_ptr<qp>>
   */
  asio::awaitable<std::shared_ptr<qp>> connect();
};

using qp_connector = basic_qp_connector<cq_poller>;
using native_qp_connector = basic_qp_connector<native_cq_poller>;

} // namespace rdmapp
