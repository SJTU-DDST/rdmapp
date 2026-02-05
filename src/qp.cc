#include "rdmapp/qp.h"

#include "rdmapp/detail/logger.h"
#include "rdmapp/detail/serdes.h"
#include "rdmapp/error.h"
#include "rdmapp/mr.h"
#include "rdmapp/pd.h"
#include "rdmapp/srq.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <infiniband/verbs.h>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <strings.h>
#include <utility>
#include <vector>

namespace rdmapp {

std::atomic<uint32_t> basic_qp::next_sq_psn = 1;

basic_qp::basic_qp(uint16_t remote_lid, uint32_t remote_qpn,
                   uint32_t remote_psn, union ibv_gid remote_gid,
                   std::shared_ptr<pd> pd, std::shared_ptr<cq> cq,
                   std::shared_ptr<srq> srq, qp_config config)
    : basic_qp(remote_lid, remote_qpn, remote_psn, remote_gid, pd, cq, cq, srq,
               config) {}

basic_qp::basic_qp(uint16_t remote_lid, uint32_t remote_qpn,
                   uint32_t remote_psn, union ibv_gid remote_gid,
                   std::shared_ptr<pd> pd, std::shared_ptr<cq> recv_cq,
                   std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq,
                   qp_config config)
    : basic_qp(pd, recv_cq, send_cq, srq, config) {
  rtr(remote_lid, remote_qpn, remote_psn, remote_gid);
  rts();
}

basic_qp::basic_qp(std::shared_ptr<rdmapp::pd> pd, std::shared_ptr<cq> cq,
                   std::shared_ptr<srq> srq, qp_config config)
    : basic_qp(pd, cq, cq, srq, config) {}

basic_qp::basic_qp(std::shared_ptr<rdmapp::pd> pd, std::shared_ptr<cq> recv_cq,
                   std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq,
                   qp_config config)
    : qp_(nullptr), pd_(pd), recv_cq_(recv_cq), send_cq_(send_cq), srq_(srq),
      config_(config) {
  create();
  init();
}

std::vector<std::byte> &basic_qp::user_data() { return user_data_; }

std::shared_ptr<pd> basic_qp::pd_ptr() const { return pd_; }

std::vector<std::byte> basic_qp::serialize() const {
  std::vector<std::byte> buffer;
  auto it = std::back_inserter(buffer);
  detail::serialize(pd_->device_ptr()->lid(), it);
  detail::serialize(qp_->qp_num, it);
  detail::serialize(sq_psn_, it);
  detail::serialize(static_cast<uint32_t>(user_data_.size()), it);
  detail::serialize(pd_->device_ptr()->gid(), it);
  std::copy(user_data_.cbegin(), user_data_.cend(), it);
  return buffer;
}

void basic_qp::create() {
  struct ibv_qp_init_attr qp_init_attr{};
  qp_init_attr.qp_type = IBV_QPT_RC;
  qp_init_attr.recv_cq = recv_cq_->cq_;
  qp_init_attr.send_cq = send_cq_->cq_;
  qp_init_attr.cap.max_recv_sge = 1;
  qp_init_attr.cap.max_send_sge = 1;
  qp_init_attr.cap.max_recv_wr = config_.max_recv_wr;
  qp_init_attr.cap.max_send_wr = config_.max_send_wr;
  qp_init_attr.sq_sig_all = 0;
  qp_init_attr.qp_context = this;

  if (srq_ != nullptr) {
    qp_init_attr.srq = srq_->srq_;
    raw_srq_ = srq_->srq_;
    post_recv_fn = &basic_qp::post_recv_srq;
  } else {
    post_recv_fn = &basic_qp::post_recv_rq;
  }

  qp_ = ::ibv_create_qp(pd_->pd_, &qp_init_attr);
  check_ptr(qp_, "failed to create qp");
  sq_psn_ = next_sq_psn.fetch_add(1);
  log::trace("created qp {} lid={} qpn={} psn={}", log::fmt::ptr(qp_),
             pd_->device_ptr()->lid(), qp_->qp_num, sq_psn_);
}

void basic_qp::init() {
  struct ibv_qp_attr qp_attr = {};
  ::bzero(&qp_attr, sizeof(qp_attr));
  qp_attr.qp_state = IBV_QPS_INIT;
  qp_attr.pkey_index = 0;
  qp_attr.port_num = pd_->device_ptr()->port_num();
  qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
  try {
    check_rc(::ibv_modify_qp(qp_, &qp_attr,
                             IBV_QP_STATE | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS |
                                 IBV_QP_PKEY_INDEX),
             "failed to transition qp to init state");
  } catch (const std::exception &e) {
    log::error("{}", e.what());
    qp_ = nullptr;
    destroy();
    throw;
  }
}

void basic_qp::rtr(uint16_t remote_lid, uint32_t remote_qpn,
                   uint32_t remote_psn, union ibv_gid remote_gid) {
  struct ibv_qp_attr qp_attr = {};
  ::bzero(&qp_attr, sizeof(qp_attr));
  qp_attr.qp_state = IBV_QPS_RTR;
  qp_attr.path_mtu = IBV_MTU_4096;
  qp_attr.dest_qp_num = remote_qpn;
  qp_attr.rq_psn = remote_psn;
  qp_attr.max_dest_rd_atomic = 16;
  qp_attr.min_rnr_timer = 12;
  qp_attr.ah_attr.is_global = 1;
  qp_attr.ah_attr.grh.dgid = remote_gid;
  qp_attr.ah_attr.grh.sgid_index = pd_->device_->gid_index_;
  qp_attr.ah_attr.grh.hop_limit = 16;
  qp_attr.ah_attr.dlid = remote_lid;
  qp_attr.ah_attr.sl = 0;
  qp_attr.ah_attr.src_path_bits = 0;
  qp_attr.ah_attr.port_num = pd_->device_ptr()->port_num();

  try {
    check_rc(::ibv_modify_qp(qp_, &qp_attr,
                             IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                                 IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                 IBV_QP_MIN_RNR_TIMER |
                                 IBV_QP_MAX_DEST_RD_ATOMIC),
             "failed to transition qp to rtr state");
  } catch (const std::exception &e) {
    log::error("{}", e.what());
    qp_ = nullptr;
    destroy();
    throw;
  }
}

void basic_qp::rts() {
  struct ibv_qp_attr qp_attr = {};
  ::bzero(&qp_attr, sizeof(qp_attr));
  qp_attr.qp_state = IBV_QPS_RTS;
  qp_attr.timeout = 14;
  qp_attr.retry_cnt = 7;
  qp_attr.rnr_retry = 7;
  qp_attr.max_rd_atomic = 16;
  qp_attr.sq_psn = sq_psn_;

  try {
    check_rc(::ibv_modify_qp(qp_, &qp_attr,
                             IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                                 IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                                 IBV_QP_MAX_QP_RD_ATOMIC),
             "failed to transition qp to rts state");
  } catch (std::exception const &e) {
    log::error("{}", e.what());
    qp_ = nullptr;
    destroy();
    throw;
  }
}

void basic_qp::post_send(struct ibv_send_wr const &send_wr,
                         struct ibv_send_wr *&bad_send_wr) {
  log::trace("post send wr_id={:#x} addr={:#x}", send_wr.wr_id,
             send_wr.sg_list->addr);
  check_rc(::ibv_post_send(qp_, const_cast<struct ibv_send_wr *>(&send_wr),
                           &bad_send_wr),
           "failed to post send");
}

void basic_qp::post_recv(struct ibv_recv_wr const &recv_wr,
                         struct ibv_recv_wr *&bad_recv_wr) const {
  (this->*(post_recv_fn))(recv_wr, bad_recv_wr);
}

void basic_qp::post_recv_rq(struct ibv_recv_wr const &recv_wr,
                            struct ibv_recv_wr *&bad_recv_wr) const {
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("post recv wr_id={:#x} sg_list={} addr={:#x}", recv_wr.wr_id,
             log::fmt::ptr(recv_wr.sg_list),
             recv_wr.sg_list ? recv_wr.sg_list->addr : 0x0);
#endif
  check_rc(::ibv_post_recv(qp_, const_cast<struct ibv_recv_wr *>(&recv_wr),
                           &bad_recv_wr),
           "failed to post recv");
}

void basic_qp::post_recv_srq(struct ibv_recv_wr const &recv_wr,
                             struct ibv_recv_wr *&bad_recv_wr) const {
  check_rc(::ibv_post_srq_recv(raw_srq_,
                               const_cast<struct ibv_recv_wr *>(&recv_wr),
                               &bad_recv_wr),
           "failed to post srq recv");
}

basic_qp::send_awaitable::send_awaitable(std::shared_ptr<basic_qp> qp,
                                         std::span<std::byte> buffer,
                                         enum ibv_wr_opcode opcode)
    : qp_(qp), local_mr_(std::make_unique<local_mr>(
                   qp->pd_->reg_mr(buffer.data(), buffer.size_bytes()))),
      local_mr_view_(*local_mr_), remote_mr_view_(), state_(opcode) {}

basic_qp::send_awaitable::send_awaitable(std::shared_ptr<basic_qp> qp,
                                         std::span<std::byte> buffer,
                                         enum ibv_wr_opcode opcode,
                                         mr_view remote_mr)
    : qp_(qp), local_mr_(std::make_unique<local_mr>(
                   qp->pd_->reg_mr(buffer.data(), buffer.size_bytes()))),
      local_mr_view_(*local_mr_), remote_mr_view_(remote_mr), state_(opcode) {}

basic_qp::send_awaitable::send_awaitable(std::shared_ptr<basic_qp> qp,
                                         std::span<std::byte> buffer,
                                         enum ibv_wr_opcode opcode,
                                         mr_view remote_mr, uint32_t imm)
    : qp_(qp), local_mr_(std::make_unique<local_mr>(
                   qp->pd_->reg_mr(buffer.data(), buffer.size_bytes()))),
      local_mr_view_(*local_mr_), remote_mr_view_(remote_mr), imm_(imm),
      state_(opcode) {}

basic_qp::send_awaitable::send_awaitable(std::shared_ptr<basic_qp> qp,
                                         std::span<std::byte> buffer,
                                         enum ibv_wr_opcode opcode,
                                         mr_view remote_mr, uint64_t add)
    : qp_(qp), local_mr_(std::make_unique<local_mr>(
                   qp->pd_->reg_mr(buffer.data(), buffer.size_bytes()))),
      local_mr_view_(*local_mr_), remote_mr_view_(remote_mr), compare_add_(add),
      state_(opcode) {}

basic_qp::send_awaitable::send_awaitable(std::shared_ptr<basic_qp> qp,
                                         std::span<std::byte> buffer,
                                         enum ibv_wr_opcode opcode,
                                         mr_view remote_mr, uint64_t compare,
                                         uint64_t swap)
    : qp_(qp), local_mr_(std::make_unique<local_mr>(
                   qp->pd_->reg_mr(buffer.data(), buffer.size_bytes()))),
      local_mr_view_(*local_mr_), remote_mr_view_(remote_mr),
      compare_add_(compare), swap_(swap), state_(opcode) {}

basic_qp::send_awaitable::send_awaitable(std::weak_ptr<basic_qp> qp,
                                         mr_view local_mr,
                                         enum ibv_wr_opcode opcode)
    : qp_(qp), local_mr_view_(local_mr), remote_mr_view_(), state_(opcode) {}

basic_qp::send_awaitable::send_awaitable(std::weak_ptr<basic_qp> qp,
                                         mr_view local_mr,
                                         enum ibv_wr_opcode opcode,
                                         mr_view remote_mr)
    : qp_(qp), local_mr_view_(local_mr), remote_mr_view_(remote_mr),
      state_(opcode) {}

basic_qp::send_awaitable::send_awaitable(std::weak_ptr<basic_qp> qp,
                                         mr_view local_mr,
                                         enum ibv_wr_opcode opcode,
                                         mr_view remote_mr, uint32_t imm)
    : qp_(qp), local_mr_view_(local_mr), remote_mr_view_(remote_mr), imm_(imm),
      state_(opcode) {}

basic_qp::send_awaitable::send_awaitable(std::weak_ptr<basic_qp> qp,
                                         mr_view local_mr,
                                         enum ibv_wr_opcode opcode,
                                         mr_view remote_mr, uint64_t add)
    : qp_(qp), local_mr_view_(local_mr), remote_mr_view_(remote_mr),
      compare_add_(add), state_(opcode) {}

basic_qp::send_awaitable::send_awaitable(std::weak_ptr<basic_qp> qp,
                                         mr_view local_mr,
                                         enum ibv_wr_opcode opcode,
                                         mr_view remote_mr,

                                         uint64_t compare, uint64_t swap)
    : qp_(qp), local_mr_view_(local_mr), remote_mr_view_(remote_mr),
      compare_add_(compare), swap_(swap), state_(opcode) {}

static inline struct ibv_sge fill_local_sge(mr_view const &mr) {
  struct ibv_sge sge = {};
  sge.addr = reinterpret_cast<uint64_t>(mr.addr());
  sge.length = mr.length();
  sge.lkey = mr.lkey();
  return sge;
}

bool basic_qp::send_awaitable::await_ready() const noexcept { return false; }

basic_qp::operation_state::operation_state() noexcept
    : wr_opcode(std::nullopt) {}

basic_qp::operation_state::operation_state(enum ibv_wr_opcode opcode) noexcept
    : wr_opcode(opcode) {}

uintptr_t basic_qp::operation_state::wr_id() const noexcept {
  return reinterpret_cast<uintptr_t>(this);
}

void basic_qp::operation_state::set_from_wc(ibv_wc const &wc) noexcept {
#ifdef RDMAPP_BUILD_DEBUG
  validate();
#endif

  wc_status = wc.status;
  wc_flags = wc.wc_flags;

  if (!wr_opcode) { // it is recv, no need to check wr_opcode
    byte_len = wc.byte_len;
    imm_data = wc.imm_data;
    return;
  }

  switch (*wr_opcode) {
  case IBV_WR_RDMA_WRITE_WITH_IMM:
  case IBV_WR_RDMA_WRITE:
  case IBV_WR_SEND:
    break;
  default:
    byte_len = wc.byte_len;
  }
}

void basic_qp::operation_state::resume() const {
#ifdef RDMAPP_BUILD_DEBUG
  validate();
#endif
  coro_handle.resume();
}

bool basic_qp::send_awaitable::await_suspend(
    std::coroutine_handle<> h) noexcept {
  state_.coro_handle = h;
  uintptr_t const wr_id = state_.wr_id();
  return this->suspend(wr_id);
}

bool basic_qp::send_awaitable::suspend(uintptr_t wr_id) noexcept {
  auto send_sge = fill_local_sge(local_mr_view_);

  struct ibv_send_wr send_wr = {};
  struct ibv_send_wr *bad_send_wr = nullptr;
  assert(state_.wr_opcode);
  send_wr.opcode = *state_.wr_opcode;
  send_wr.next = nullptr;
  send_wr.num_sge = 1;
  send_wr.wr_id = reinterpret_cast<uint64_t>(wr_id);
  send_wr.send_flags = IBV_SEND_SIGNALED;
  send_wr.sg_list = &send_sge;
  if (is_rdma()) {
    assert(remote_mr_view_.addr() != nullptr);
    send_wr.wr.rdma.remote_addr =
        reinterpret_cast<uint64_t>(remote_mr_view_.addr());
    send_wr.wr.rdma.rkey = remote_mr_view_.rkey();
    if (state_.wr_opcode == IBV_WR_RDMA_WRITE_WITH_IMM) {
      send_wr.imm_data = imm_;
    }
    state_.byte_len = local_mr_view_.length();
  } else if (is_atomic()) {
    assert(remote_mr_view_.addr() != nullptr);
    send_wr.wr.atomic.remote_addr =
        reinterpret_cast<uint64_t>(remote_mr_view_.addr());
    send_wr.wr.atomic.rkey = remote_mr_view_.rkey();
    send_wr.wr.atomic.compare_add = compare_add_;
    if (state_.wr_opcode == IBV_WR_ATOMIC_CMP_AND_SWP) {
      send_wr.wr.atomic.swap = swap_;
    }
  }

  try {
    std::shared_ptr<basic_qp> qp = qp_.lock();
    if (!qp) [[unlikely]] {
      exception_ = std::make_exception_ptr(
          std::runtime_error("post_send: qp expired, use_count=0"));
      return false;
    }
    qp->post_send(send_wr, bad_send_wr);
  } catch (std::runtime_error &e) {
    exception_ = std::make_exception_ptr(e);
    return false;
  }
  return true;
}

constexpr bool basic_qp::send_awaitable::is_rdma() const {
  return state_.wr_opcode == IBV_WR_RDMA_READ ||
         state_.wr_opcode == IBV_WR_RDMA_WRITE ||
         state_.wr_opcode == IBV_WR_RDMA_WRITE_WITH_IMM;
}

constexpr bool basic_qp::send_awaitable::is_atomic() const {
  return state_.wr_opcode == IBV_WR_ATOMIC_CMP_AND_SWP ||
         state_.wr_opcode == IBV_WR_ATOMIC_FETCH_AND_ADD;
}

basic_qp::send_result basic_qp::send_awaitable::resume() const {
  check_wc_status(state_.wc_status, "failed to send");
  /* ref: https://www.rdmamojo.com/2013/02/15/ibv_poll_cq/
   * byte_len: The number of bytes transferred. Relevant if the Receive Queue
   * for incoming Send or RDMA Write with immediate operations. This value
   * doesn't include the length of the immediate data, if such exists. Relevant
   * in the Send Queue for RDMA Read and Atomic operations.
   * */
  /* this field is undefined for rdma write*/
  return state_.byte_len;
}

uint32_t basic_qp::send_awaitable::await_resume() const {
  if (exception_) [[unlikely]] {
    std::rethrow_exception(exception_);
  }
  return resume();
}

basic_qp::recv_awaitable::recv_awaitable(std::shared_ptr<basic_qp> qp,
                                         std::span<std::byte> buffer)
    : qp_(qp), local_mr_(std::make_unique<local_mr>(
                   qp->pd_->reg_mr(buffer.data(), buffer.size()))),
      local_mr_view_(*local_mr_), state_() {}

basic_qp::recv_awaitable::recv_awaitable(std::weak_ptr<basic_qp> qp,
                                         mr_view local_mr)
    : qp_(qp), local_mr_view_(local_mr), state_() {}

bool basic_qp::recv_awaitable::await_ready() const noexcept { return false; }

bool basic_qp::recv_awaitable::suspend(uintptr_t wr_id) noexcept {
  ibv_sge recv_sge, *recv_sge_list{nullptr};
  int num_sge{0};
  if (local_mr_view_) {
    recv_sge = fill_local_sge(local_mr_view_);
    recv_sge_list = &recv_sge;
    num_sge = 1;
  }

  struct ibv_recv_wr recv_wr = {};
  struct ibv_recv_wr *bad_recv_wr = nullptr;
  recv_wr.next = nullptr;
  recv_wr.wr_id = wr_id;
  recv_wr.num_sge = num_sge;
  recv_wr.sg_list = recv_sge_list;

  try {
    std::shared_ptr<basic_qp> qp = qp_.lock();
    if (!qp) [[unlikely]] {
      exception_ = std::make_exception_ptr(
          std::runtime_error("post_recv: qp expired, use_count=0"));
      return false;
    }
    qp->post_recv(recv_wr, bad_recv_wr);
  } catch (std::runtime_error &e) {
    exception_ = std::make_exception_ptr(e);
    return false;
  }
  return true;
}

bool basic_qp::recv_awaitable::await_suspend(
    std::coroutine_handle<> h) noexcept {
  state_.coro_handle = h;
  uintptr_t const wr_id = state_.wr_id();
  return this->suspend(wr_id);
}

basic_qp::recv_result basic_qp::recv_awaitable::resume() const {
  check_wc_status(state_.wc_status, "failed to recv");
  if (state_.wc_flags & IBV_WC_WITH_IMM) {
#ifdef RDMAPP_BUILD_DEBUG
    log::trace("recv resume: imm: wr_id={:#x} imm={}", state_.wr_id(),
               state_.imm_data);
#endif
    return std::make_pair(state_.byte_len, state_.imm_data);
  }
  return std::make_pair(state_.byte_len, std::nullopt);
}

basic_qp::recv_result basic_qp::recv_awaitable::await_resume() const {
  if (exception_) [[unlikely]] {
    std::rethrow_exception(exception_);
  }
  return resume();
}

void basic_qp::destroy() {
  if (qp_ == nullptr) [[unlikely]] {
    return;
  }
  err();
  if (auto rc = ::ibv_destroy_qp(qp_); rc != 0) [[unlikely]] {
    log::error("failed to destroy qp {}: {}", log::fmt::ptr(qp_),
               strerror(errno));
  } else {
    log::trace("destroyed qp {}", log::fmt::ptr(qp_));
  }
}

void basic_qp::err() {
  struct ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_ERR;

  if (ibv_modify_qp(qp_, &attr, IBV_QP_STATE)) {
    log::error("failed to modify qp({}) to ERROR, error: {}\n",
               log::fmt::ptr(qp_), strerror(errno));
  } else {
    log::trace("qp({}) set to ERORR", log::fmt::ptr(qp_));
  }
}

std::exception_ptr basic_qp::recv_awaitable::unhandled_exception() const {
  return exception_;
}

std::exception_ptr basic_qp::send_awaitable::unhandled_exception() const {
  return exception_;
}

basic_qp::~basic_qp() { destroy(); }

} // namespace rdmapp
