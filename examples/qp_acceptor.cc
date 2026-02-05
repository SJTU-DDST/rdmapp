#include <spdlog/spdlog.h>

#include "qp_acceptor.h"

#include "qp_transmission.h"
#include "rdmapp/scheduler.h"
#include <cppcoro/net/ipv4_address.hpp>
#include <cppcoro/net/ipv4_endpoint.hpp>
#include <cppcoro/net/socket.hpp>
#include <stdexcept>
#include <sys/socket.h> // Added for setsockopt, SOL_SOCKET, SO_REUSEADDR

namespace rdmapp {

static auto config_socket(cppcoro::net::socket &socket) {
  int fd = socket.native_handle();
  int opt = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    throw std::runtime_error("config socket failed");
  }
}

template <typename cq_poller_t>
basic_qp_acceptor<cq_poller_t>::basic_qp_acceptor(
    cppcoro::io_service &io_service,
    std::shared_ptr<rdmapp::scheduler> scheduler, uint16_t port,
    std::shared_ptr<pd> pd, std::shared_ptr<srq> srq, ConnConfig config)
    : pd_(pd), srq_(srq), port_(port), io_service_(io_service),
      config_(std::move(config)),
      acceptor_socket_(cppcoro::net::socket::create_tcpv4(io_service_)),
      scheduler_(scheduler) {
  try {
    config_socket(acceptor_socket_);
    acceptor_socket_.bind(
        cppcoro::net::ipv4_endpoint(cppcoro::net::ipv4_address(), port));
    acceptor_socket_.listen();
    spdlog::info("qp_acceptor: bind and listen: port={}", port_);
  } catch (std::exception &e) {
    spdlog::error("qp_acceptor: failed to bind and listen: error={}", e.what());
    std::terminate();
  }
}

template <typename cq_poller_t>
auto basic_qp_acceptor<cq_poller_t>::accept_qp(
    cppcoro::net::socket &socket, std::shared_ptr<rdmapp::cq> send_cq,
    std::shared_ptr<rdmapp::cq> recv_cq)
    -> cppcoro::task<std::shared_ptr<rdmapp::qp>> {
  auto remote_qp = co_await recv_qp(socket);
  auto local_qp = std::make_shared<rdmapp::qp>(
      remote_qp.header.lid, remote_qp.header.qp_num, remote_qp.header.sq_psn,
      remote_qp.header.gid, pd_, recv_cq, send_cq, srq_, config_.queue_pair_config);
  local_qp->user_data() = std::move(remote_qp.user_data);
  co_await send_qp(*local_qp, socket);
  co_return local_qp;
}

template <typename cq_poller_t>
auto basic_qp_acceptor<cq_poller_t>::accept()
    -> cppcoro::task<std::shared_ptr<rdmapp::qp>> {
  cppcoro::net::socket socket = cppcoro::net::socket::create_tcpv4(io_service_);
  co_await acceptor_socket_.accept(socket);
  co_return co_await accept_qp(socket, alloc_cq(), alloc_cq());
}

template <typename cq_poller_t>
std::shared_ptr<rdmapp::cq> basic_qp_acceptor<cq_poller_t>::alloc_cq() {
  auto cq = std::make_shared<rdmapp::cq>(pd_->device_ptr(), config_.cq_size);
  pollers_.emplace_back(cq, scheduler_);
  return cq;
}

template <typename cq_poller_t>
auto basic_qp_acceptor<cq_poller_t>::close() noexcept -> void {
  try {
    acceptor_socket_.close();
    spdlog::warn("qp_acceptor: closed");
  } catch (std::exception &e) {
    spdlog::error("qp_acceptor: error close, msg={}", e.what());
  }
}

template class basic_qp_acceptor<rdmapp::native_cq_poller>;

} // namespace rdmapp
