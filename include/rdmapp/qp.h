#pragma once
#ifndef RDAMPP_QP_HPP__
#define RDAMPP_QP_HPP__

#ifdef RDMAPP_ASIO_COROUTINE
#include <asio/awaitable.hpp>
#endif
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <infiniband/verbs.h>

#include "rdmapp/completion_token.h"
#include "rdmapp/cq.h"
#include "rdmapp/device.h"
#include "rdmapp/mr.h"
#include "rdmapp/pd.h"
#include "rdmapp/srq.h"

#include "rdmapp/detail/noncopyable.h"
#include "rdmapp/detail/serdes.h"

namespace rdmapp {

/**
 * @brief Configuration for a Queue Pair.
 */
struct qp_config {
  uint32_t max_send_wr;
  uint32_t max_recv_wr;
};

/**
 * @brief Returns the default configuration for a Queue Pair.
 * @return The default configuration.
 */
static constexpr qp_config default_qp_config() { return {256, 256}; }

/**
 * @brief A structure holding the deserialized data required to connect to a
 * remote Queue Pair. This is typically received from a remote peer to establish
 * a connection.
 */
struct deserialized_qp {
  /**
   * @brief The header portion of the serialized QP data.
   */
  struct qp_header {
    /**
     * @brief The total size of the serialized header in bytes.
     */
    static constexpr size_t kSerializedSize =
        sizeof(uint16_t) + 3 * sizeof(uint32_t) + sizeof(union ibv_gid);
    /// The Local Identifier (LID) of the remote port.
    uint16_t lid;
    /// The Queue Pair Number (QPN) of the remote QP.
    uint32_t qp_num;
    /// The starting Sequence Number (PSN) for the Send Queue of the remote QP.
    uint32_t sq_psn;
    /// The size of the user-defined data that follows the header.
    uint32_t user_data_size;
    /// The Global Identifier (GID) of the remote port.
    union ibv_gid gid;
  } header;

  /**
   * @brief Deserializes QP information from a given iterator.
   * @tparam It An iterator type pointing to a sequence of bytes.
   * @param it The iterator to read the serialized data from.
   * @return A `deserialized_qp` object populated with the data.
   */
  template <class It> static deserialized_qp deserialize(It it) {
    deserialized_qp des_qp;
    detail::deserialize(it, des_qp.header.lid);
    detail::deserialize(it, des_qp.header.qp_num);
    detail::deserialize(it, des_qp.header.sq_psn);
    detail::deserialize(it, des_qp.header.user_data_size);
    detail::deserialize(it, des_qp.header.gid);
    return des_qp;
  }

  /// Optional user-defined data that was sent along with the QP header.
  std::vector<std::byte> user_data;
};

namespace detail {
/**
 * @brief A type alias to select the return type of an awaitable operation based
 * on a completion token.
 * @tparam Token The completion token type (`use_asio_awaitable_t` or
 * `use_native_awaitable_t`).
 * @tparam Awaitable The native awaitable type used when
 * `use_native_awaitable_t` is specified.
 * @tparam Result The result type of the asynchronous operation.
 */
template <typename Token, typename Awaitable, typename Result>
using awaitable_return_t =
#ifdef RDMAPP_ASIO_COROUTINE
    std::conditional_t<
        std::is_same_v<std::remove_cvref_t<Token>, use_asio_awaitable_t>,
        asio::awaitable<Result>, Awaitable>;
#else
    Awaitable;
#endif

/**
 * @brief A helper function to cast away the constness from a `std::span`.
 * @note This is potentially unsafe and should only be used when an underlying
 * API requires a non-const buffer for an operation that is known to be
 * read-only.
 * @tparam T The element type of the span.
 * @param s The `std::span<const T>` to cast.
 * @return A `std::span<T>` pointing to the same data.
 */
template <typename T> auto span_const_cast(std::span<const T> s) noexcept {
  return std::span<T>(const_cast<T *>(s.data()), s.size());
}
} // namespace detail

/**
 * @brief A C++ abstraction for an InfiniBand Queue Pair (QP).
 *
 * This class wraps the `ibv_qp` object and provides a modern, coroutine-based
 * interface for performing RDMA operations such as send, receive, read, write,
 * and atomics. It manages the QP lifecycle and simplifies resource handling
 * through RAII and smart pointers.
 *
 * @tparam ResumeStrategy The strategy for resuming coroutines after an
 * operation completes. See `rdmapp::qp_strategy`.
 */
class basic_qp : public noncopyable,
                 public std::enable_shared_from_this<basic_qp> {
  static std::atomic<uint32_t> next_sq_psn;
  struct ibv_qp *qp_;
  struct ibv_srq *raw_srq_;
  uint32_t sq_psn_;
  void (basic_qp::*post_recv_fn)(struct ibv_recv_wr const &recv_wr,
                                 struct ibv_recv_wr *&bad_recv_wr) const;
  std::shared_ptr<pd> pd_;
  std::shared_ptr<cq> recv_cq_;
  std::shared_ptr<cq> send_cq_;
  std::shared_ptr<srq> srq_;
  qp_config const config_;
  std::vector<std::byte> user_data_;

  /**
   * @brief Creates a new Queue Pair. The Queue Pair will be in the RESET state.
   */
  void create();

  /**
   * @brief Initializes the Queue Pair. The Queue Pair will be transitioned to
   * the INIT state.
   */
  void init();

  /**
   * @brief Destroys the underlying `ibv_qp` resource.
   */
  void destroy();

public:
  struct operation_state {
    operation_state() noexcept;
    operation_state(enum ibv_wr_opcode opcode) noexcept;
#ifdef RDMAPP_BUILD_DEBUG
    static constexpr uint32_t kMagic1 = 0x190514;
    static constexpr uint32_t kMagic2 = 0xABCABC;
    uint32_t magic1{kMagic1};
    uint32_t magic2{kMagic2};
    void validate() const noexcept {
      assert(magic1 == kMagic1);
      assert(magic2 == kMagic2);
    }
#endif
    std::optional<enum ibv_wr_opcode> const wr_opcode;
    std::coroutine_handle<> coro_handle{};
    enum ibv_wc_status wc_status {};
    unsigned int wc_flags{};
    uint32_t imm_data{};
    uint32_t byte_len{};

    void set_from_wc(ibv_wc const &wc) noexcept;
    uintptr_t wr_id() const noexcept;
    void resume() const;
  };

  /// The result type for send-like operations, typically representing the
  /// number of bytes transferred.
  using send_result = uint32_t;

  /**
   * @brief An awaitable object representing a pending send, RDMA, or atomic
   * operation.
   *
   * This object is returned by methods like `send`, `write`, `read`, and
   * `compare_and_swap`. It is both a native C++20 awaitable and convertible to
   * an `asio::awaitable`.
   */
  class [[nodiscard]] send_awaitable
      : public std::enable_shared_from_this<send_awaitable> {
    friend basic_qp;
    std::weak_ptr<basic_qp> qp_;
    std::unique_ptr<local_mr> local_mr_ ;
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
    /* for rdma_write, wc.byte_len is undefined*/
    /* hence we store the write bytes here as return*/
    operation_state state_;

  public:
    send_awaitable(std::weak_ptr<basic_qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode);
    send_awaitable(std::weak_ptr<basic_qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode, mr_view remote_mr);
    send_awaitable(std::weak_ptr<basic_qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode, mr_view remote_mr, uint32_t imm);
    send_awaitable(std::weak_ptr<basic_qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode, mr_view remote_mr, uint64_t add);
    send_awaitable(std::weak_ptr<basic_qp> qp, mr_view local_mr,
                   enum ibv_wr_opcode opcode, mr_view remote_mr,
                   uint64_t compare, uint64_t swap);
    send_awaitable(std::shared_ptr<basic_qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode); // send
    send_awaitable(std::shared_ptr<basic_qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode,
                   mr_view remote_mr); // write
    send_awaitable(std::shared_ptr<basic_qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode, mr_view remote_mr,
                   uint32_t imm); // write with imm
    send_awaitable(std::shared_ptr<basic_qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode, mr_view remote_mr,
                   uint64_t add); // fetch and add
    send_awaitable(std::shared_ptr<basic_qp> qp, std::span<std::byte> buffer,
                   enum ibv_wr_opcode opcode, mr_view remote_mr,
                   uint64_t compare, uint64_t swap); // cas

    // Internal methods for integration with different asynchronous models.
    bool suspend(uintptr_t wr_id) noexcept;
    send_result resume() const;

    // C++20 Coroutine Awaitable Interface
    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> h) noexcept;
    uint32_t await_resume() const;

    /// @brief Retrieves any exception that occurred during the operation.
    std::exception_ptr unhandled_exception() const;

#ifdef RDMAPP_ASIO_COROUTINE
    /// @brief Implicitly converts the object to an `asio::awaitable` for use in
    /// Asio coroutines.
    [[nodiscard]] operator asio::awaitable<send_result>() &&;
#endif

    /// @brief Checks if the operation is an RDMA operation (read or write).
    constexpr bool is_rdma() const;
    /// @brief Checks if the operation is an atomic operation.
    constexpr bool is_atomic() const;
  };

  /**
   * @brief Factory function for creating a `send_awaitable` with a specific
   * completion token.
   * @tparam CompletionToken The completion token type.
   * @tparam Args The types of arguments for the `send_awaitable` constructor.
   * @param token The completion token instance.
   * @param args The arguments to forward to the `send_awaitable` constructor.
   * @return An awaitable object whose type is determined by the
   * `CompletionToken`.
   */
  template <typename... Args, typename CompletionToken>
  requires ValidCompletionToken<CompletionToken>
  static auto make_send_awaitable(CompletionToken token [[maybe_unused]],
                                  Args &&...args) noexcept
      -> detail::awaitable_return_t<CompletionToken, send_awaitable,
                                    send_result> {
    return send_awaitable{std::forward<Args>(args)...};
  }

  /// The result type for receive operations: a pair of {bytes received,
  /// optional immediate data}.
  using recv_result = std::pair<uint32_t, std::optional<uint32_t>>;

  /**
   * @brief An awaitable object representing a pending receive operation.
   *
   * This object is returned by the `recv` method. It is both a native C++20
   * awaitable and convertible to an `asio::awaitable`.
   */
  class [[nodiscard]] recv_awaitable {
    friend basic_qp;
    std::weak_ptr<basic_qp> qp_;
    std::unique_ptr<local_mr> local_mr_;
    mr_view local_mr_view_;
    std::exception_ptr exception_;
    operation_state state_;

  public:
    recv_awaitable(recv_awaitable &&) = default;
    recv_awaitable(recv_awaitable const &) = delete;
    recv_awaitable(std::weak_ptr<basic_qp> qp, mr_view local_mr);
    recv_awaitable(std::shared_ptr<basic_qp> qp, std::span<std::byte> buffer);

    // Internal methods for integration with different asynchronous models.
    bool suspend(uintptr_t wr_id) noexcept;
    recv_result resume() const;

    // C++20 Coroutine Awaitable Interface
    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> h) noexcept;
    recv_result await_resume() const;

    /// @brief Retrieves any exception that occurred during the operation.
    std::exception_ptr unhandled_exception() const;

#ifdef RDMAPP_ASIO_COROUTINE
    /// @brief Implicitly converts the object to an `asio::awaitable` for use in
    /// Asio coroutines.
    [[nodiscard]] operator asio::awaitable<recv_result>() &&;
#endif
  };

  /**
   * @brief Factory function for creating a `recv_awaitable` with a specific
   * completion token.
   * @tparam CompletionToken The completion token type.
   * @tparam Args The types of arguments for the `recv_awaitable` constructor.
   * @param token The completion token instance.
   * @param args The arguments to forward to the `recv_awaitable` constructor.
   * @return An awaitable object whose type is determined by the
   * `CompletionToken`.
   */
  template <typename CompletionToken, typename... Args>
  requires ValidCompletionToken<CompletionToken>
  static auto make_recv_awaitable(CompletionToken token [[maybe_unused]],
                                  Args &&...args) noexcept
      -> detail::awaitable_return_t<CompletionToken, recv_awaitable,
                                    recv_result> {
    return recv_awaitable{std::forward<Args>(args)...};
  }

  /**
   * @brief Constructs a new QP and connects it to a remote peer.
   *        The QP will be in the Ready-To-Send (RTS) state upon construction.
   *
   * @param remote_lid The LID of the remote QP's port.
   * @param remote_qpn The QPN of the remote QP.
   * @param remote_psn The initial PSN of the remote QP.
   * @param remote_gid The GID of the remote QP's port.
   * @param pd The Protection Domain for this QP.
   * @param cq The Completion Queue for both send and receive completions.
   * @param srq (Optional) A Shared Receive Queue. If provided, receive WQEs
   * will be posted here.
   */
  basic_qp(const uint16_t remote_lid, const uint32_t remote_qpn,
           const uint32_t remote_psn, const union ibv_gid remote_gid,
           std::shared_ptr<pd> pd, std::shared_ptr<cq> cq,
           std::shared_ptr<srq> srq = nullptr,
           qp_config config = default_qp_config());
  /**
   * @brief Constructs a new QP and connects it to a remote peer with separate
   * CQs. The QP will be in the Ready-To-Send (RTS) state upon construction.
   *
   * @param remote_lid The LID of the remote QP's port.
   * @param remote_qpn The QPN of the remote QP.
   * @param remote_psn The initial PSN of the remote QP.
   * @param remote_gid The GID of the remote QP's port.
   * @param pd The Protection Domain for this QP.
   * @param recv_cq The Completion Queue for receive completions.
   * @param send_cq The Completion Queue for send completions.
   * @param srq (Optional) A Shared Receive Queue. If provided, receive WQEs
   * will be posted here.
   * @param config (Optional) The configuration for this QP.
   */
  basic_qp(const uint16_t remote_lid, const uint32_t remote_qpn,
           const uint32_t remote_psn, const union ibv_gid remote_gid,
           std::shared_ptr<pd> pd, std::shared_ptr<cq> recv_cq,
           std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq = nullptr,
           qp_config config = default_qp_config());
  /**
   * @brief Constructs a new QP in the INIT state.
   *        The QP is not connected and must be manually transitioned to RTR and
   * RTS.
   *
   * @param pd The Protection Domain for this QP.
   * @param cq The Completion Queue for both send and receive completions.
   * @param srq (Optional) A Shared Receive Queue. If provided, receive WQEs
   * will be posted here.
   * @param config (Optional) The configuration for this QP.
   */
  basic_qp(std::shared_ptr<pd> pd, std::shared_ptr<cq> cq,
           std::shared_ptr<srq> srq = nullptr,
           qp_config config = default_qp_config());
  /**
   * @brief Constructs a new QP in the INIT state with separate CQs.
   *        The QP is not connected and must be manually transitioned to RTR and
   * RTS.
   *
   * @param pd The Protection Domain for this QP.
   * @param recv_cq The Completion Queue for receive completions.
   * @param send_cq The Completion Queue for send completions.
   * @param srq (Optional) A Shared Receive Queue. If provided, receive WQEs
   * will be posted here.
   * @param config (Optional) The configuration for this QP.
   */
  basic_qp(std::shared_ptr<pd> pd, std::shared_ptr<cq> recv_cq,
           std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq = nullptr,
           qp_config config = default_qp_config());

  /**
   * @brief Posts a raw send Work Request (`ibv_send_wr`) to the QP's Send
   * Queue.
   *
   * @param send_wr The send work request to post.
   * @param bad_send_wr A pointer that will be set to the failed work request on
   * error.
   */
  void post_send(struct ibv_send_wr const &send_wr,
                 struct ibv_send_wr *&bad_send_wr);
  /**
   * @brief Posts a raw receive Work Request (`ibv_recv_wr`) to the appropriate
   * receive queue. This will post to the QP's own Receive Queue (RQ) or the
   * associated Shared Receive Queue (SRQ) if one was provided during
   * construction.
   *
   * @param recv_wr The receive work request to post.
   * @param bad_recv_wr A pointer that will be set to the failed work request on
   * error.
   */
  void post_recv(struct ibv_recv_wr const &recv_wr,
                 struct ibv_recv_wr *&bad_recv_wr) const;

  /**
   * @brief Sends data from a local buffer to the remote peer.
   *        The local buffer is temporarily registered as a memory region for
   * the duration of the operation.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param buffer A span over the local data buffer. It must remain valid until
   * the operation completes.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes sent.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto send(std::span<std::byte const> buffer,
                          CompletionToken token = default_completion_token) {
    return make_send_awaitable(token, this->shared_from_this(),
                               detail::span_const_cast(buffer), IBV_WR_SEND);
  }

  /**
   * @brief Writes data from a local buffer to a remote memory region.
   *        The local buffer is temporarily registered as a memory region for
   * the duration of the operation.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region to write to.
   * @param buffer A span over the local data buffer. It must remain valid until
   * the operation completes.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes
   * written.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto write(mr_view remote_mr, std::span<std::byte const> buffer,
                           CompletionToken token = default_completion_token) {
    return make_send_awaitable(token, this->shared_from_this(),
                               detail::span_const_cast(buffer),
                               IBV_WR_RDMA_WRITE, remote_mr);
  }

  /**
   * @brief Writes data from a local buffer to a remote memory region with an
   * immediate value. The local buffer is temporarily registered as a memory
   * region. The remote peer will receive the immediate value along with the
   * completion notification.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region to write to.
   * @param buffer A span over the local data buffer. It must remain valid until
   * the operation completes.
   * @param imm The 32-bit immediate value to send.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes
   * written.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto
  write_with_imm(mr_view remote_mr, std::span<std::byte const> buffer,
                 uint32_t imm,
                 CompletionToken token = default_completion_token) {
    return make_send_awaitable(token, this->shared_from_this(),
                               detail::span_const_cast(buffer),
                               IBV_WR_RDMA_WRITE_WITH_IMM, remote_mr, imm);
  }

  /**
   * @brief Reads data from a remote memory region into a local buffer.
   *        The local buffer is temporarily registered as a memory region for
   * the duration of the operation.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region to read from.
   * @param buffer A span over the local data buffer where the read data will be
   * stored. It must remain valid until the operation completes.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes read.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto read(mr_view remote_mr, std::span<std::byte> buffer,
                          CompletionToken token = default_completion_token) {
    return make_send_awaitable(token, this->shared_from_this(), buffer,
                               IBV_WR_RDMA_READ, remote_mr);
  }

  /**
   * @brief Performs an atomic Fetch-And-Add operation on a remote 64-bit value.
   *        The original value is fetched into the local buffer. The local
   * buffer is temporarily registered.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region (must be 8 bytes).
   * @param buffer A span over the local data buffer (must be 8 bytes) to store
   * the original value. It must remain valid until the operation completes.
   * @param add The value to add to the remote integer.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes fetched
   * (always 8).
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto
  fetch_and_add(mr_view remote_mr, std::span<std::byte> buffer, uint64_t add,
                CompletionToken token = default_completion_token) {
    assert(pd_->device_ptr()->is_fetch_and_add_supported());
    return make_send_awaitable(token, this->shared_from_this(), buffer,
                               IBV_WR_ATOMIC_FETCH_AND_ADD, remote_mr, add);
  }

  /**
   * @brief Performs an atomic Compare-And-Swap operation on a remote 64-bit
   * value. The original value is fetched into the local buffer. The local
   * buffer is temporarily registered.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region (must be 8 bytes).
   * @param buffer A span over the local data buffer (must be 8 bytes) to store
   * the original value. It must remain valid until the operation completes.
   * @param compare The expected value at the remote address.
   * @param swap The new value to write if the comparison succeeds.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes fetched
   * (always 8).
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto
  compare_and_swap(mr_view remote_mr, std::span<std::byte> buffer,
                   uint64_t compare, uint64_t swap,
                   CompletionToken token = default_completion_token) {
    assert(pd_->device_ptr()->is_compare_and_swap_supported());
    return make_send_awaitable(token, this->shared_from_this(), buffer,
                               IBV_WR_ATOMIC_CMP_AND_SWP, remote_mr, compare,
                               swap);
  }

  /**
   * @brief Posts a receive buffer to be filled with incoming data.
   *        The local buffer is temporarily registered as a memory region for
   * the duration of the operation.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param buffer A span over the local buffer to receive data. It must remain
   * valid until the operation completes.
   * @param token The completion token.
   * @return An awaitable object that completes with a pair containing the
   * number of bytes received and an optional immediate value.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto recv(std::span<std::byte> buffer,
                          CompletionToken token = default_completion_token) {
    return make_recv_awaitable(token, this->shared_from_this(), buffer);
  }

  /**
   * @brief Sends data from a pre-registered local memory region to the remote
   * peer.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param local_mr A view of the pre-registered local memory region. It must
   * remain valid until the operation completes.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes sent.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto send(mr_view local_mr,
                          CompletionToken token = default_completion_token) {
    return make_send_awaitable(token, this->weak_from_this(), local_mr,
                               IBV_WR_SEND);
  }

  /**
   * @brief Writes data from a pre-registered local memory region to a remote
   * memory region.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region to write to.
   * @param local_mr A view of the pre-registered local memory region. It must
   * remain valid until the operation completes.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes
   * written.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto write(mr_view remote_mr, mr_view local_mr,
                           CompletionToken token = default_completion_token) {
    return make_send_awaitable(token, this->weak_from_this(), local_mr,
                               IBV_WR_RDMA_WRITE, remote_mr);
  }

  /**
   * @brief Writes data from a pre-registered local memory region to a remote
   * memory region with an immediate value.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region to write to.
   * @param local_mr A view of the pre-registered local memory region. It must
   * remain valid until the operation completes.
   * @param imm The 32-bit immediate value to send.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes
   * written.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto
  write_with_imm(mr_view remote_mr, mr_view local_mr, uint32_t imm,
                 CompletionToken token = default_completion_token) {
    return make_send_awaitable(token, this->weak_from_this(), local_mr,
                               IBV_WR_RDMA_WRITE_WITH_IMM, remote_mr, imm);
  }

  /**
   * @brief Reads data from a remote memory region into a pre-registered local
   * memory region.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region to read from.
   * @param local_mr A view of the pre-registered local memory region to store
   * the data. It must remain valid until the operation completes.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes read.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto read(mr_view remote_mr, mr_view local_mr,
                          CompletionToken token = default_completion_token) {
    return make_send_awaitable(token, this->weak_from_this(), local_mr,
                               IBV_WR_RDMA_READ, remote_mr);
  }

  /**
   * @brief Performs an atomic Fetch-And-Add using a pre-registered local memory
   * region.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region (must be 8 bytes).
   * @param local_mr A view of the pre-registered local memory region to store
   * the original value (must be 8 bytes). It must remain valid until the
   * operation completes.
   * @param add The value to add to the remote integer.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes fetched
   * (always 8).
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto
  fetch_and_add(mr_view remote_mr, mr_view local_mr, uint64_t add,
                CompletionToken token = default_completion_token) {
    assert(pd_->device_ptr()->is_fetch_and_add_supported());
    return make_send_awaitable(token, this->weak_from_this(), local_mr,
                               IBV_WR_ATOMIC_FETCH_AND_ADD, remote_mr, add);
  }

  /**
   * @brief Performs an atomic Compare-And-Swap using a pre-registered local
   * memory region.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param remote_mr A view of the remote memory region (must be 8 bytes).
   * @param local_mr A view of the pre-registered local memory region to store
   * the original value (must be 8 bytes). It must remain valid until the
   * operation completes.
   * @param compare The expected value at the remote address.
   * @param swap The new value to write if the comparison succeeds.
   * @param token The completion token.
   * @return An awaitable object that completes with the number of bytes fetched
   * (always 8).
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]] auto
  compare_and_swap(mr_view remote_mr, mr_view local_mr, uint64_t compare,
                   uint64_t swap,
                   CompletionToken &&token = default_completion_token) {
    assert(pd_->device_ptr()->is_compare_and_swap_supported());
    return make_send_awaitable(token, this->weak_from_this(), local_mr,
                               IBV_WR_ATOMIC_CMP_AND_SWP, remote_mr, compare,
                               swap);
  }

  /**
   * @brief Posts a pre-registered memory region as a receive buffer.
   *
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param local_mr A view of the pre-registered local memory region to receive
   * data into. It must remain valid until the operation completes.
   * @param token The completion token.
   * @return An awaitable object that completes with a pair containing the
   * number of bytes received and an optional immediate value.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]]
  auto recv(mr_view local_mr,
            CompletionToken token = default_completion_token) {
    return make_recv_awaitable(token, this->weak_from_this(), local_mr);
  }

  /**
   * @brief Waits for a receive completion without a pre-posted buffer (if using
   * SRQ with zero-copy semantics).
   * @note This is typically used with SRQs where buffers are managed
   * separately.
   * @tparam CompletionToken The completion token type that determines the
   * return type.
   * @param token The completion token.
   * @return An awaitable object that completes when a message is received.
   */
  template <typename CompletionToken = decltype(default_completion_token)>
  [[nodiscard]]
  auto recv(CompletionToken token = default_completion_token)
  requires ValidCompletionToken<CompletionToken>
  {
    return make_recv_awaitable(token, this->weak_from_this(), mr_view{});
  }

  /**
   * @brief Serializes the QP's connection data into a byte vector for
   * transmission to a remote peer.
   *
   * @return A `std::vector<std::byte>` containing the serialized data.
   */
  std::vector<std::byte> serialize() const;

  /**
   * @brief Provides access to user-defined data associated with this QP.
   *
   * This can be used to store application-specific metadata.
   *
   * @return A reference to the `std::vector<std::byte>` holding the user data.
   */
  std::vector<std::byte> &user_data();

  /**
   * @brief Gets the Protection Domain (`pd`) associated with this QP.
   *
   * @return A `std::shared_ptr<pd>` to the Protection Domain.
   */
  std::shared_ptr<pd> pd_ptr() const;

  /**
   * @brief Destroys the Queue Pair and releases all associated `ibverbs`
   * resources.
   */
  ~basic_qp();

  /**
   * @brief Transitions the Queue Pair from INIT to Ready-To-Receive (RTR)
   * state.
   *
   * @param remote_lid The LID of the remote QP's port.
   * @param remote_qpn The QPN of the remote QP.
   * @param remote_psn The initial PSN of the remote QP.
   * @param remote_gid The GID of the remote QP's port.
   */
  void rtr(uint16_t remote_lid, uint32_t remote_qpn, uint32_t remote_psn,
           union ibv_gid remote_gid);

  /**
   * @brief Transitions the Queue Pair from RTR to Ready-To-Send (RTS) state.
   */
  void rts();

  /**
   * @brief Transitions the Queue Pair to the ERROR state.
   */
  void err();

private:
  /**
   * @brief Posts a receive work request to the QP's own Receive Queue (RQ).
   * @param recv_wr The work request to post.
   * @param bad_recv_wr A pointer that will be set to the failed work request on
   * error.
   */
  void post_recv_rq(struct ibv_recv_wr const &recv_wr,
                    struct ibv_recv_wr *&bad_recv_wr) const;
  /**
   * @brief Posts a receive work request to the associated Shared Receive Queue
   * (SRQ).
   * @param recv_wr The work request to post.
   * @param bad_recv_wr A pointer that will be set to the failed work request on
   * error.
   */
  void post_recv_srq(struct ibv_recv_wr const &recv_wr,
                     struct ibv_recv_wr *&bad_recv_wr) const;
};

/**
 * @brief A convenience alias for a `basic_qp` using the default `AtPoller`
 * resumption strategy.
 */
using qp = basic_qp;

} // namespace rdmapp

/**
 * @example helloworld.cc
 * This is an example of how to create a Queue Pair connected with a remote peer
 * and perform send/recv/read/write/atomic operations on the QP.
 */
/**
 * @example write_bw.cc
 * This is an example of testing the bandwidth of a Queue Pair using RDMA write.
 * It also demonstrates how to run multiple tasks in background concurrently and
 * wait for them to complete.
 */
/**
 * @example rpc.cc
 * This is a simple example of implementing an RPC-like pattern with a Queue
 * Pair using write_with_imm/recv operations.
 */
#endif
