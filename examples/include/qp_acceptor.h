#pragma once
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <cstdint>

#include <rdmapp/detail/noncopyable.h>
#include <rdmapp/device.h>
#include <rdmapp/pd.h>
#include <rdmapp/qp.h>

namespace rdmapp {

/**
 * @brief This class is used to accept incoming connections and queue pairs.
 *
 */
class qp_acceptor : public noncopyable {
  std::shared_ptr<pd> pd_;
  std::shared_ptr<cq> recv_cq_;
  std::shared_ptr<cq> send_cq_;
  std::shared_ptr<srq> srq_;
  std::shared_ptr<asio::io_context> io_ctx_;
  asio::ip::tcp::acceptor acceptor_;

public:
  /**
   * @brief Construct a new acceptor object.
   *
   * @param loop The event loop to use.
   * @param port The port to listen on.
   * @param recv_cq The recv completion queue to use for incoming Queue Pairs.
   * @param send_cq The send completion queue to use for incoming Queue Pairs.
   * @param srq () The shared receive queue to use for incoming Queue
   * Pairs.
   */
  qp_acceptor(std::shared_ptr<asio::io_context> io_ctx, uint16_t port,
              std::shared_ptr<pd> pd, std::shared_ptr<cq> cq,
              std::shared_ptr<srq> srq = nullptr);

  /**
   * @brief Construct a new acceptor object.
   *
   * @param loop The event loop to use.
   * @param port The port to listen on.
   * @param pd The protection domain for all new Queue Pairs.
   * @param recv_cq The recv completion queue to use for incoming Queue Pairs.
   * @param send_cq The send completion queue to use for incoming Queue Pairs.
   * @param srq (Optional) The shared receive queue to use for incoming Queue
   * Pairs.
   */
  qp_acceptor(std::shared_ptr<asio::io_context> io_ctx, uint16_t port,
              std::shared_ptr<pd> pd, std::shared_ptr<cq> recv_cq,
              std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq = nullptr);

  /**
   * @brief This function is used to accept an incoming connection and queue
   * pair. This should be called in a loop.
   *
   * @return task<std::shared_ptr<qp>> A completion task that returns a shared
   * pointer to the new queue pair. It will be in the RTS state.
   */
  asio::awaitable<std::shared_ptr<qp>> accept();

  ~qp_acceptor() = default;
};

} // namespace rdmapp
