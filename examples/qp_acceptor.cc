#include "qp_acceptor.h"

#include "qp_transmission.h"

namespace rdmapp {
qp_acceptor::qp_acceptor(std::shared_ptr<pd> pd, std::shared_ptr<cq> cq,
                         std::shared_ptr<srq> srq)
    : qp_acceptor(pd, cq, cq, srq) {}

qp_acceptor::qp_acceptor(std::shared_ptr<pd> pd, std::shared_ptr<cq> recv_cq,
                         std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq)
    : pd_(pd), recv_cq_(recv_cq), send_cq_(send_cq), srq_(srq) {}

asio::awaitable<std::shared_ptr<qp>>
qp_acceptor::accept(asio::ip::tcp::socket socket) {
  auto remote_qp = co_await recv_qp(socket);
  auto local_qp = std::make_shared<rdmapp::qp>(
      remote_qp.header.lid, remote_qp.header.qp_num, remote_qp.header.sq_psn,
      remote_qp.header.gid, pd_, recv_cq_, send_cq_);
  local_qp->user_data() = std::move(remote_qp.user_data);
  co_await send_qp(*local_qp, socket);
  co_return local_qp;
}

} // namespace rdmapp
