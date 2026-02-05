#pragma once

#include "qp_transmission.h"
#include "rdmapp/scheduler.h"
#include <cppcoro/io_service.hpp>
#include <cppcoro/net/socket.hpp>
#include <cppcoro/task.hpp>
#include <cstdint>
#include <list>
#include <memory>
#include <rdmapp/cq.h>
#include <rdmapp/cq_poller.h>
#include <rdmapp/detail/noncopyable.h>
#include <rdmapp/device.h>
#include <rdmapp/pd.h>
#include <rdmapp/qp.h>
#include <rdmapp/srq.h>

namespace rdmapp {

/**
 * @brief This class is used to accept incoming connections and queue pairs.
 *
 */
template <typename cq_poller_t> class basic_qp_acceptor : public noncopyable {
  std::shared_ptr<pd> pd_;
  std::shared_ptr<srq> srq_;
  std::list<cq_poller_t> pollers_;
  uint16_t const port_;
  cppcoro::io_service &io_service_;
  ConnConfig const config_;
  cppcoro::net::socket acceptor_socket_;
  std::shared_ptr<rdmapp::scheduler> scheduler_;

  std::shared_ptr<rdmapp::cq> alloc_cq();

  auto accept_qp(cppcoro::net::socket &socket,
                 std::shared_ptr<rdmapp::cq> send_cq,
                 std::shared_ptr<rdmapp::cq> recv_cq)
      -> cppcoro::task<std::shared_ptr<rdmapp::qp>>;

public:
  /**
   * @brief Construct a new acceptor object.
   *
   * @param io_service The cppcoro::io_service to use.
   * @param port The port to listen on.
   * @param pd The protection domain for all new Queue Pairs.
   * @param srq () The shared receive queue to use for incoming Queue
   * Pairs.
   */
  basic_qp_acceptor(cppcoro::io_service &io_service,
                    std::shared_ptr<rdmapp::scheduler> scheduler, uint16_t port,
                    std::shared_ptr<pd> pd, std::shared_ptr<srq> srq = nullptr,
                    ConnConfig config = {});

  /**
   * @brief This function is used to accept an incoming connection and queue
   * pair. This should be called in a loop.
   *
   * @return cppcoro::task<std::shared_ptr<rdmapp::qp>> An cppcoro::task that
   * returns a shared pointer to the new queue pair. It will be in the RTS
   * state.
   */
  auto accept() -> cppcoro::task<std::shared_ptr<rdmapp::qp>>;

  auto close() noexcept -> void;

  ~basic_qp_acceptor() = default;
};

using qp_acceptor = basic_qp_acceptor<cq_poller>;
using native_qp_acceptor = basic_qp_acceptor<native_cq_poller>;

} // namespace rdmapp
