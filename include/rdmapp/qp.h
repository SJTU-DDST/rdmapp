#pragma once

#include <asio/awaitable.hpp>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <infiniband/verbs.h>

#include "rdmapp/cq.h"
#include "rdmapp/device.h"
#include "rdmapp/executor.h"
#include "rdmapp/mr.h"
#include "rdmapp/pd.h"
#include "rdmapp/srq.h"

#include "rdmapp/detail/noncopyable.h"
#include "rdmapp/detail/serdes.h"

namespace rdmapp {

struct deserialized_qp {
  struct qp_header {
    static constexpr size_t kSerializedSize =
        sizeof(uint16_t) + 3 * sizeof(uint32_t) + sizeof(union ibv_gid);
    uint16_t lid;
    uint32_t qp_num;
    uint32_t sq_psn;
    uint32_t user_data_size;
    union ibv_gid gid;
  } header;
  template <class It> static deserialized_qp deserialize(It it) {
    deserialized_qp des_qp;
    detail::deserialize(it, des_qp.header.lid);
    detail::deserialize(it, des_qp.header.qp_num);
    detail::deserialize(it, des_qp.header.sq_psn);
    detail::deserialize(it, des_qp.header.user_data_size);
    detail::deserialize(it, des_qp.header.gid);
    return des_qp;
  }
  std::vector<uint8_t> user_data;
};

/**
 * @brief This class is an abstraction of an Infiniband Queue Pair.
 *
 */
class qp : public noncopyable, public std::enable_shared_from_this<qp> {
  static std::atomic<uint32_t> next_sq_psn;
  struct ibv_qp *qp_;
  struct ibv_srq *raw_srq_;
  uint32_t sq_psn_;
  void (qp::*post_recv_fn)(struct ibv_recv_wr const &recv_wr,
                           struct ibv_recv_wr *&bad_recv_wr) const;

  std::shared_ptr<pd> pd_;
  std::shared_ptr<cq> recv_cq_;
  std::shared_ptr<cq> send_cq_;
  std::shared_ptr<srq> srq_;
  std::vector<uint8_t> user_data_;

  /**
   * @brief Creates a new Queue Pair. The Queue Pair will be in the RESET state.
   *
   */
  void create();

  /**
   * @brief Initializes the Queue Pair. The Queue Pair will be in the INIT
   * state.
   *
   */
  void init();

  void destroy();

public:
  // TODO: maybe use send_result in the future
  using send_result = uint32_t;

  class send_awaitable : public std::enable_shared_from_this<send_awaitable> {
    friend class qp;
    std::shared_ptr<qp> qp_;
    std::shared_ptr<local_mr> local_mr_ [[maybe_unused]];
    mr_view local_mr_view_;
    mr_view remote_mr_view_;
    std::exception_ptr exception_;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wgnu-anonymous-struct"
#pragma GCC diagnostic ignored "-Wnested-anon-types"
#endif
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
    union {
      struct {
        uint64_t compare_add_;
        uint64_t swap_;
      };
      uint32_t imm_;
    };

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    struct ibv_wc wc_;
    const enum ibv_wr_opcode opcode_;

  public:
    send_awaitable(std::shared_ptr<qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode);
    send_awaitable(std::shared_ptr<qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode, mr_view remote_mr);
    send_awaitable(std::shared_ptr<qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode, mr_view remote_mr, uint32_t imm);
    send_awaitable(std::shared_ptr<qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode, mr_view remote_mr, uint64_t add);
    send_awaitable(std::shared_ptr<qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode, mr_view remote_mr,
                   uint64_t compare, uint64_t swap);

    send_awaitable(std::shared_ptr<qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode); // send
    send_awaitable(std::shared_ptr<qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode,
                   mr_view remote_mr); // write
    send_awaitable(std::shared_ptr<qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode, mr_view remote_mr,
                   uint32_t imm); // write with imm

    send_awaitable(std::shared_ptr<qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode, mr_view remote_mr,
                   uint64_t add); // fetch and add
    send_awaitable(std::shared_ptr<qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode, mr_view remote_mr,
                   uint64_t compare, uint64_t swap); // cas

    // NOTE: support asio::async_compose for asio::awaitable
    // how to use: https://github.com/chriskohlhoff/asio/issues/795
    bool suspend(executor::callback_ptr callback) noexcept;
    send_result resume() const;

    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> h) noexcept;
    uint32_t await_resume() const;
    constexpr bool is_rdma() const;
    constexpr bool is_atomic() const;
  };

  [[nodiscard]] asio::awaitable<send_result>
  make_asio_awaitable(std::unique_ptr<send_awaitable> awaitable);

  using recv_result = std::pair<uint32_t, std::optional<uint32_t>>;
  class recv_awaitable {
    friend class qp;
    std::shared_ptr<qp> qp_;
    std::shared_ptr<local_mr> local_mr_;
    mr_view local_mr_view_;
    std::exception_ptr exception_;
    struct ibv_wc wc_;
    enum ibv_wr_opcode opcode [[maybe_unused]];

  public:
    recv_awaitable(recv_awaitable &&) = default;
    recv_awaitable(recv_awaitable const &) = delete;
    recv_awaitable(std::shared_ptr<qp> qp, mr_view local_mr);
    recv_awaitable(std::shared_ptr<qp> qp, std::span<std::byte> buffer);

    bool suspend(executor::callback_ptr fn) noexcept;
    recv_result resume() const;

    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> h) noexcept;
    recv_result await_resume() const;
  };

  [[nodiscard]] asio::awaitable<recv_result>
  make_asio_awaitable(std::unique_ptr<recv_awaitable> awaitable);
  /**
   * @brief Construct a new qp object. The Queue Pair will be created with the
   * given remote Queue Pair parameters. Once constructed, the Queue Pair will
   * be in the RTS state.
   *
   * @param remote_lid The LID of the remote Queue Pair.
   * @param remote_qpn The QPN of the remote Queue Pair.
   * @param remote_psn The PSN of the remote Queue Pair.
   * @param pd The protection domain of the new Queue Pair.
   * @param cq The completion queue of both send and recv work completions.
   * @param srq (Optional) If set, all recv work requests will be posted to this
   * SRQ.
   */
  qp(const uint16_t remote_lid, const uint32_t remote_qpn,
     const uint32_t remote_psn, const union ibv_gid remote_gid,
     std::shared_ptr<pd> pd, std::shared_ptr<cq> cq,
     std::shared_ptr<srq> srq = nullptr);

  /**
   * @brief Construct a new qp object. The Queue Pair will be created with the
   * given remote Queue Pair parameters. Once constructed, the Queue Pair will
   * be in the RTS state.
   *
   * @param remote_lid The LID of the remote Queue Pair.
   * @param remote_qpn The QPN of the remote Queue Pair.
   * @param remote_psn The PSN of the remote Queue Pair.
   * @param pd The protection domain of the new Queue Pair.
   * @param recv_cq The completion queue of recv work completions.
   * @param send_cq The completion queue of send work completions.
   * @param srq (Optional) If set, all recv work requests will be posted to this
   * SRQ.
   */
  qp(const uint16_t remote_lid, const uint32_t remote_qpn,
     const uint32_t remote_psn, const union ibv_gid remote_gid,
     std::shared_ptr<pd> pd, std::shared_ptr<cq> recv_cq,
     std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq = nullptr);

  /**
   * @brief Construct a new qp object. The constructed Queue Pair will be in
   * INIT state.
   *
   * @param pd The protection domain of the new Queue Pair.
   * @param cq The completion queue of both send and recv work completions.
   * @param srq (Optional) If set, all recv work requests will be posted to this
   * SRQ.
   */
  qp(std::shared_ptr<pd> pd, std::shared_ptr<cq> cq,
     std::shared_ptr<srq> srq = nullptr);

  /**
   * @brief Construct a new qp object. The constructed Queue Pair will be in
   * INIT state.
   *
   * @param pd The protection domain of the new Queue Pair.
   * @param recv_cq The completion queue of recv work completions.
   * @param send_cq The completion queue of send work completions.
   * @param srq (Optional) If set, all recv work requests will be posted to this
   * SRQ.
   */
  qp(std::shared_ptr<pd> pd, std::shared_ptr<cq> recv_cq,
     std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq = nullptr);

  /**
   * @brief This function is used to post a send work request to the Queue Pair.
   *
   * @param recv_wr The work request to post.
   * @param bad_recv_wr A pointer to a work request that will be set to the
   * first work request that failed to post.
   */
  void post_send(struct ibv_send_wr const &send_wr,
                 struct ibv_send_wr *&bad_send_wr);

  /**
   * @brief This function is used to post a recv work request to the Queue Pair.
   * It will be posted to either RQ or SRQ depending on whether or not SRQ is
   * set.
   *
   * @param recv_wr The work request to post.
   * @param bad_recv_wr A pointer to a work request that will be set to the
   * first work request that failed to post.
   */
  void post_recv(struct ibv_recv_wr const &recv_wr,
                 struct ibv_recv_wr *&bad_recv_wr) const;

  /**
   * @brief This method sends local buffer to remote. The address will be
   * registered as a memory region first and then deregistered upon completion.
   *
   * @param buffer span to local buffer. It should be valid until completion.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data sent.
   */
  [[nodiscard]] asio::awaitable<send_result>
  send(std::span<std::byte const> buffer);

  /**
   * @brief This method writes local buffer to a remote memory region. The local
   * buffer will be registered as a memory region first and then deregistered
   * upon completion.
   *
   * @param remote_mr Remote memory region handle.
   * @param buffer span to local buffer. It should be valid until completion.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data written.
   */
  [[nodiscard]] asio::awaitable<send_result>
  write(mr_view remote_mr, std::span<std::byte const> buffer);

  /**
   * @brief This method writes local buffer to a remote memory region with an
   * immediate value. The local buffer will be registered as a memory region
   * first and then deregistered upon completion.
   *
   * @param remote_mr Remote memory region handle.
   * @param buffer span to local buffer. It should be valid until
   * completion.
   * @param imm The immediate value.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data written.
   */
  [[nodiscard]] asio::awaitable<send_result>
  write_with_imm(mr_view remote_mr, std::span<std::byte const> buffer,
                 uint32_t imm);

  /**
   * @brief This method reads to local buffer from a remote memory region. The
   * local buffer will be registered as a memory region first and then
   * deregistered upon completion.
   *
   * @param remote_mr Remote memory region handle.
   * @param buffer span to local buffer. It should be valid until
   * completion.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data read.
   */
  [[nodiscard]] asio::awaitable<send_result> read(mr_view remote_mr,
                                                  std::span<std::byte> buffer);

  /**
   * @brief This method performs an atomic fetch-and-add operation on the
   * given remote memory region. The local buffer will be registered as a
   * memory region first and then deregistered upon completion.
   *
   * @param remote_mr Remote memory region handle.
   * @param buffer span to local buffer. It should be valid until
   * completion.
   * @param add The delta.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data sent.
   */
  [[nodiscard]] asio::awaitable<send_result>
  fetch_and_add(mr_view remote_mr, std::span<std::byte> buffer, uint64_t add);

  /**
   * @brief This method performs an atomic compare-and-swap operation on the
   * given remote memory region. The local buffer will be registered as a
   * memory region first and then deregistered upon completion.
   *
   * @param remote_mr Remote memory region handle.
   * @param buffer span to local buffer. It should be valid until
   * completion.
   * @param compare The expected old value.
   * @param swap The desired new value.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data sent.
   */
  [[nodiscard]] asio::awaitable<send_result>
  compare_and_swap(mr_view remote_mr, std::span<std::byte> buffer,
                   uint64_t compare, uint64_t swap);

  /**
   * @brief This method posts a recv request on the queue pair. The buffer
   * will be filled with data received. The local buffer will be registered as
   * a memory region first and then deregistered upon completion.
   * @param buffer span to local buffer. It should be valid until
   * completion.
   * @return asio::awaitable<recv_result> A coroutine returning
   * std::pair<uint32_t, std::optional<uint32_t>>, with first indicating the
   * length of received data, and second indicating the immediate value if any.
   */
  [[nodiscard]] asio::awaitable<recv_result> recv(std::span<std::byte> buffer);

  /**
   * @brief This function sends a registered local memory region to remote.
   *
   * @param local_mr Registered local memory region, which should be valid until
   * completion.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data sent.
   */
  [[nodiscard]] asio::awaitable<send_result> send(mr_view local_mr);

  /**
   * @brief This function writes a registered local memory region to remote.
   *
   * @param remote_mr Remote memory region handle.
   * @param local_mr Registered local memory region, which should be valid until
   * completion.
   * controlled by a smart pointer.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data written.
   */
  [[nodiscard]] asio::awaitable<send_result> write(mr_view remote_mr,
                                                   mr_view local_mr);

  /**
   * @brief This function writes a registered local memory region to remote
   * with an immediate value.
   *
   * @param remote_mr Remote memory region handle.
   * @param local_mr Registered local memory region, which should be valid until
   * completion.
   * @param imm The immediate value.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data written.
   */
  [[nodiscard]] asio::awaitable<send_result>
  write_with_imm(mr_view remote_mr, mr_view local_mr, uint32_t imm);

  /**
   * @brief This function reads to local memory region from remote.
   *
   * @param remote_mr Remote memory region handle.
   * @param local_mr Registered local memory region, which should be valid until
   * completion.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data read.
   */
  [[nodiscard]] asio::awaitable<send_result> read(mr_view remote_mr,
                                                  mr_view local_mr);

  /**
   * @brief This function performs an atomic fetch-and-add operation on the
   * given remote memory region.
   *
   * @param remote_mr Remote memory region handle.
   * @param local_mr Registered local memory region, which should be valid until
   * completion.
   * @param add The delta.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data sent.
   */
  [[nodiscard]] asio::awaitable<send_result>
  fetch_and_add(mr_view remote_mr, mr_view local_mr, uint64_t add);

  /**
   * @brief This function performs an atomic compare-and-swap operation on the
   * given remote memory region.
   *
   * @param remote_mr Remote memory region handle.
   * @param local_mr Registered local memory region, which should be valid until
   * completion.
   * @param compare The expected old value.
   * @param swap The desired new value.
   * @return asio::awaitable<send_result> A coroutine returning result of the
   * data sent.
   */
  [[nodiscard]] asio::awaitable<send_result> compare_and_swap(mr_view remote_mr,
                                                              mr_view local_mr,
                                                              uint64_t compare,
                                                              uint64_t swap);

  /**
   * @brief This function posts a recv request on the queue pair. The buffer
   * will be filled with data received.
   *
   * @param local_mr Registered local memory region, which should be valid until
   * completion.
   * @return asio::awaitable<recv_result> A coroutine returning std::pair<uint32_t,
   * std::optional<uint32_t>>, with first indicating the length of received
   * data, and second indicating the immediate value if any.
   */
  [[nodiscard]] asio::awaitable<recv_result> recv(mr_view local_mr = mr_view());

  /**
   * @brief This function serializes a Queue Pair prepared to be sent to a
   * buffer.
   *
   * @return std::vector<uint8_t> The serialized QP.
   */
  std::vector<uint8_t> serialize() const;

  /**
   * @brief This function provides access to the extra user data of the Queue
   * Pair.
   *
   * @return std::vector<uint8_t>& The extra user data.
   */
  std::vector<uint8_t> &user_data();

  /**
   * @brief This function provides access to the Protection Domain of the
   * Queue Pair.
   *
   * @return std::shared_ptr<pd> Pointer to the PD.
   */
  std::shared_ptr<pd> pd_ptr() const;
  ~qp();

  /**
   * @brief This function transitions the Queue Pair to the RTR state.
   *
   * @param remote_lid The remote LID.
   * @param remote_qpn The remote QPN.
   * @param remote_psn The remote PSN.
   * @param remote_gid The remote GID.
   */
  void rtr(uint16_t remote_lid, uint32_t remote_qpn, uint32_t remote_psn,
           union ibv_gid remote_gid);

  /**
   * @brief This function transitions the Queue Pair to the RTS state.
   *
   */
  void rts();

  /**
   * @brief This function transitions the Queue Pair to the ERROR state.
   *
   */
  void err();

private:
  /**
   * @brief This function posts a recv request on the Queue Pair's own RQ.
   *
   * @param recv_wr
   * @param bad_recv_wr
   */
  void post_recv_rq(struct ibv_recv_wr const &recv_wr,
                    struct ibv_recv_wr *&bad_recv_wr) const;

  /**
   * @brief This function posts a send request on the Queue Pair's SRQ.
   *
   * @param recv_wr
   * @param bad_recv_wr
   */
  void post_recv_srq(struct ibv_recv_wr const &recv_wr,
                     struct ibv_recv_wr *&bad_recv_wr) const;
};

} // namespace rdmapp

/** \example helloworld.cc
 * This is an example of how to create a Queue Pair connected with a remote peer
 * and perform send/recv/read/write/atomic operations on the QP.
 */

/** \example send_bw.cc
 * This is an example of testing the bandwidth of a Queue Pair using send/recv.
 * It also demonstrates how to run multiple tasks in background concurrently and
 * wait for them to complete.
 */
