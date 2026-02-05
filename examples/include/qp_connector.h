#pragma once

#include "qp_transmission.h"
#include <cppcoro/io_service.hpp>
#include <cppcoro/net/socket.hpp>
#include <cppcoro/task.hpp>
#include <cstdint>
#include <list>
#include <memory>
#include <rdmapp/cq.h>
#include <rdmapp/cq_poller.h>
#include <rdmapp/detail/noncopyable.h>
#include <rdmapp/pd.h>
#include <rdmapp/qp.h>
#include <rdmapp/srq.h>
#include <span>
#include <string_view>

namespace rdmapp {

/**
 * @brief This class is used to actively connect to a remote endpoint and
 * establish a Queue Pair.
 *
 */
template <typename cq_poller_t> class basic_qp_connector : public noncopyable {
  std::shared_ptr<pd> pd_;
  std::shared_ptr<srq> srq_;
  std::list<cq_poller_t> pollers_;
  cppcoro::io_service &io_service_;
  ConnConfig const config_;
  std::shared_ptr<rdmapp::scheduler> scheduler_;

  std::shared_ptr<rdmapp::cq> alloc_cq();

  auto from_socket(cppcoro::net::socket &socket,
                   std::span<const std::byte> userdata)
      -> cppcoro::task<std::shared_ptr<rdmapp::qp>>;

public:
  /**
   * @brief Construct a new connector object.
   *
   * @param io_service The cppcoro::io_service to use.
   * @param pd The rdma protection domain.
   * @param srq (Optional) The shared receive queue to use for new Queue
   * Pairs.
   */
  basic_qp_connector(cppcoro::io_service &io_service,
                     std::shared_ptr<rdmapp::scheduler> scheduler,
                     std::shared_ptr<pd> pd, std::shared_ptr<srq> srq = nullptr,
                     ConnConfig config = {});

  /**
   * @brief This function is used to connect to a remote endpoint and
   * establish a Queue Pair.
   *
   * @param hostname The hostname to connect to.
   * @param port The port to connect to.
   * @param userdata (Optional) user data to send.
   *
   * @return cppcoro::task<std::shared_ptr<rdmapp::qp>>
   */
  auto connect(std::string_view hostname, uint16_t port,
               std::span<const std::byte> userdata = {})
      -> cppcoro::task<std::shared_ptr<rdmapp::qp>>;

  ~basic_qp_connector() = default;
};

using qp_connector = basic_qp_connector<cq_poller>;
using native_qp_connector = basic_qp_connector<native_cq_poller>;

} // namespace rdmapp
