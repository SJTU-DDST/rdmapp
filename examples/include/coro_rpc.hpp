#pragma once
#include <atomic>
#include <concurrentqueue.h> // moodycamel
#include <coroutine>
#include <cppcoro/async_mutex.hpp>
#include <cppcoro/async_scope.hpp>
#include <cppcoro/schedule_on.hpp>
#include <cppcoro/static_thread_pool.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/when_all.hpp>
#include <cstdint>
#include <exception>
#include <span>
#include <spdlog/spdlog.h>
#include <vector>

#include "rdmapp/mr.h"
#include <rdmapp/qp.h>

inline constexpr size_t kSendMsgSize = 256;
inline constexpr size_t kRecvMsgSize = 4096;

struct RpcHeader {
  uint64_t req_id;
  uint32_t payload_len;
  uint32_t padding;
};

inline constexpr size_t kSendBufferSize = kSendMsgSize + sizeof(RpcHeader);
inline constexpr size_t kRecvBufferSize = kRecvMsgSize + sizeof(RpcHeader);

// inline constexpr uintptr_t kRpcStatusCompleted = 0x1;

struct RpcSlot {
  std::atomic<uintptr_t> waiter{0};
  std::span<std::byte> user_resp_buffer{};
  size_t actual_len = 0;
  uint64_t expected_req_id = 0; // 关键：存储完整的编码后的 ID
};

struct RpcResponseAwaitable {
  RpcSlot &slot;
  constexpr bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) noexcept {
    slot.waiter.store(uintptr_t(h.address()));
  }
  size_t await_resume() noexcept { return slot.actual_len; }
};

class RpcClient {
  size_t max_inflight_;
  std::shared_ptr<rdmapp::qp> qp_;

  std::vector<std::byte> send_buffer_pool_;
  rdmapp::local_mr send_mr_;
  std::vector<std::byte> recv_buffer_pool_;
  rdmapp::local_mr recv_mr_;

  std::vector<RpcSlot> slots_;

  moodycamel::ConcurrentQueue<uint32_t> free_slots_;

  std::atomic<uint64_t> global_seq_{0};
  std::jthread worker_;

public:
  RpcClient(std::shared_ptr<rdmapp::qp> qp, size_t recv_depth = 512)
      : max_inflight_(recv_depth), qp_(qp),
        send_buffer_pool_(max_inflight_ * kSendBufferSize),
        send_mr_(qp->pd_ptr()->reg_mr(send_buffer_pool_.data(),
                                      send_buffer_pool_.size())),
        recv_buffer_pool_(max_inflight_ * kRecvBufferSize),
        recv_mr_(qp->pd_ptr()->reg_mr(recv_buffer_pool_.data(),
                                      recv_buffer_pool_.size())),
        slots_(max_inflight_), free_slots_(max_inflight_ * 2),
        worker_(&RpcClient::start_recv_workers, this) {

    // 初始化空闲索引池
    for (uint32_t i = 0; i < max_inflight_; ++i) {
      free_slots_.enqueue(i);
    }

    spdlog::info("client: initialized with {} slots", max_inflight_);
  }

  void start_recv_workers() {
    cppcoro::async_scope scope;
    for (size_t i = 0; i < max_inflight_; ++i) {
      scope.spawn(recv_worker(i));
    }
    cppcoro::sync_wait(scope.join());
  }

  cppcoro::task<size_t> call(std::span<const std::byte> req_data,
                             std::span<std::byte> resp_buffer) {

    uint32_t slot_idx;
    int spin_cnt = 0;
    while (!free_slots_.try_dequeue(slot_idx)) {
      spin_cnt++;
      if (spin_cnt > 10'000) {
        spdlog::warn("spin too much time, cnt={}", spin_cnt);
      }
    }

    // 2. 生成并编码 ID
    uint64_t seq = global_seq_.fetch_add(1);
    // 高 32 位 seq，低 32 位 slot_idx
    uint64_t req_id = (seq << 32) | static_cast<uint64_t>(slot_idx);

    RpcSlot &slot = slots_[slot_idx];
    slot.waiter.store(0);
    slot.user_resp_buffer = resp_buffer;
    slot.expected_req_id = req_id;

    // 3. 准备数据并发送
    size_t send_offset = slot_idx * kSendBufferSize;
    auto send_slice_mr = rdmapp::mr_view(send_mr_, send_offset,
                                         sizeof(RpcHeader) + req_data.size());

    RpcHeader *header =
        reinterpret_cast<RpcHeader *>(send_slice_mr.span().data());
    header->req_id = req_id;
    header->payload_len = req_data.size();
    std::memcpy(send_slice_mr.span().data() + sizeof(RpcHeader),
                req_data.data(), req_data.size());

    size_t nbytes = 0;
    try {
      co_await qp_->send(send_slice_mr, rdmapp::use_native_awaitable);
      // 4. 等待响应
      nbytes = co_await RpcResponseAwaitable{slot};
    } catch (const std::exception &e) {
      spdlog::error("client: RPC failed: {}", e.what());
      // 异常处理：如果不处理，Slot 会丢失
    }

    // 5. 归还 Slot
    free_slots_.enqueue(slot_idx);

    co_return nbytes;
  }

private:
  cppcoro::task<void> recv_worker(size_t worker_idx) {
    spdlog::info("recv_worker[{}] started", worker_idx);
    size_t offset = worker_idx * kRecvBufferSize;
    while (true) {
      auto recv_slice_mr = rdmapp::mr_view(recv_mr_, offset, kRecvBufferSize);
      try {
        auto [nbytes, _] =
            co_await qp_->recv(recv_slice_mr, rdmapp::use_native_awaitable);

        assert(nbytes >= sizeof(RpcHeader));

        if (nbytes < sizeof(RpcHeader))
          continue;

        auto buffer_ptr = recv_buffer_pool_.data() + offset;
        auto header = reinterpret_cast<RpcHeader *>(buffer_ptr);

        uint64_t recv_id = header->req_id;
        // 解码：取低 32 位作为索引
        uint32_t slot_idx = static_cast<uint32_t>(recv_id & 0xFFFFFFFF);

        if (slot_idx >= max_inflight_) {
          spdlog::error("client: invalid slot_idx decoded: {}", slot_idx);
          continue;
        }

        RpcSlot &slot = slots_[slot_idx];

        if (slot.expected_req_id != recv_id) [[unlikely]] {
          spdlog::error("client: received bad packet: mismatch req_id: "
                        "expected={} get={}",
                        slot.expected_req_id, recv_id);
          std::terminate();
        }
        // 拷贝数据
        size_t payload_len = header->payload_len;
        assert(payload_len != 0);
        size_t copy_len =
            std::min((size_t)payload_len, slot.user_resp_buffer.size());
        std::memcpy(slot.user_resp_buffer.data(),
                    buffer_ptr + sizeof(RpcHeader), copy_len);

        slot.actual_len = copy_len;

        uintptr_t w;
        while ((w = slot.waiter.load()) == 0)
          ;
        auto h =
            std::coroutine_handle<>::from_address(reinterpret_cast<void *>(w));
        h.resume();

      } catch (const std::exception &e) {
        spdlog::error("client: recv worker error: {}", e.what());
        break;
      }
    }
  }
};

class RpcClientMux {
  std::vector<std::unique_ptr<RpcClient>> clients_;
  std::atomic<unsigned int> selector_;

public:
  RpcClientMux(std::vector<std::shared_ptr<rdmapp::qp>> qp,
               size_t recv_depth = 512) {
    for (auto q : qp) {
      clients_.emplace_back(std::make_unique<RpcClient>(q, recv_depth));
    }
  }
  cppcoro::task<size_t> call(std::span<const std::byte> req_data,
                             std::span<std::byte> resp_buffer) {
    return clients_[selector_.fetch_add(1, std::memory_order_relaxed) %
                    clients_.size()]
        ->call(req_data, resp_buffer);
  }
};

static inline constexpr int kBufferCnt = 1024;
static inline constexpr int kBufferSize = 4096;
inline std::vector<std::byte> global_disk_pool_{kBufferSize * kBufferCnt,
                                                std::byte{0xEE}};
inline std::atomic<int> g_disk_pool_cnt{0};

inline std::size_t default_handler(std::span<std::byte> payload
                                   [[maybe_unused]],
                                   std::span<std::byte> resp) noexcept {
  const size_t offset = g_disk_pool_cnt++ % kBufferCnt * kBufferSize;
  auto const from =
      std::span<std::byte>(global_disk_pool_.begin() + offset, kBufferSize);
  assert(from.size() <= resp.size());
  std::copy_n(from.begin(), from.size(), resp.begin());
  return from.size();
}

class RpcServer {

  using Handler = std::size_t (*)(std::span<std::byte> payload,
                                  std::span<std::byte> resp);

  Handler h_;
  size_t recv_depth_;
  std::shared_ptr<rdmapp::qp> qp_;
  cppcoro::static_thread_pool tp_{4};

  std::vector<std::byte> recv_buffer_pool_;
  rdmapp::local_mr recv_mr_;
  std::vector<std::byte> send_buffer_pool_;
  rdmapp::local_mr send_mr_;

public:
  RpcServer(std::shared_ptr<rdmapp::qp> qp, size_t recv_depth = 1024,
            Handler h = default_handler)
      : h_(h), recv_depth_(recv_depth), qp_(qp),
        recv_buffer_pool_(recv_depth_ * kSendBufferSize),
        recv_mr_(qp->pd_ptr()->reg_mr(recv_buffer_pool_.data(),
                                      recv_buffer_pool_.size())),
        send_buffer_pool_(recv_depth_ * kRecvBufferSize),
        send_mr_(qp->pd_ptr()->reg_mr(send_buffer_pool_.data(),
                                      send_buffer_pool_.size()))

  {
    spdlog::info("server: initialized with {} concurrent workers", recv_depth_);
  }

  // 入口：启动所有 Worker 并保持运行
  cppcoro::task<void> run() {
    std::vector<cppcoro::task<void>> workers;
    workers.reserve(recv_depth_);

    // 启动 N 个并发循环，瞬间填满 RQ
    for (size_t i = 0; i < recv_depth_; ++i) {
      workers.emplace_back(server_worker(i));
    }

    // 等待所有协程（实际上是无限循环）
    co_await cppcoro::when_all(std::move(workers));
  }

private:
  // 单个 Worker：负责一个 Slot 的 接收 -> 处理 -> 发送 循环
  cppcoro::task<void> server_worker(size_t idx) {
    size_t recv_offset = idx * kSendBufferSize;
    size_t send_offset = idx * kRecvBufferSize;
    spdlog::info("server_worker {} spawn", idx);
    auto recv_mr = rdmapp::mr_view(recv_mr_, recv_offset, kSendBufferSize);
    auto send_mr = rdmapp::mr_view(send_mr_, send_offset, kRecvBufferSize);

    while (true) {
      // 1. 准备接收视口 (View)
      // 使用 mr_view 构造切片，避免重复注册 [1]

      // 2. 挂起等待请求
      // 此时 Buffer 归 NIC 写入，CPU 不可动
      auto [nbytes, _] =
          co_await qp_->recv(recv_mr, rdmapp::use_native_awaitable);

      if (nbytes < sizeof(RpcHeader)) {
        spdlog::warn("server: recv too small packet: size={} expected={}",
                     nbytes, sizeof(RpcHeader));
        continue; // 重新投递 Recv
      }

      // 3. 调度到业务线程池
      // 此时数据已在 Buffer，切换到 CPU 密集型线程处理
      co_await tp_.schedule();

      auto recv_view = rdmapp::mr_view(recv_mr_, recv_offset, nbytes);

      // ---------------- 业务逻辑区域 ----------------
      auto *header = reinterpret_cast<RpcHeader *>(recv_mr.addr());
      assert(header->payload_len + sizeof(RpcHeader) == recv_view.length());
      if (header->payload_len + sizeof(RpcHeader) != recv_view.length()) {
        spdlog::error("server: header payload len mismatch");
      }
      auto payload = recv_view.span().subspan<sizeof(RpcHeader)>();

      auto *resp_header = reinterpret_cast<RpcHeader *>(send_mr.addr());
      size_t resp_payload_len =
          h_(payload, send_mr.span().subspan<sizeof(RpcHeader)>());
      resp_header->req_id = header->req_id;
      resp_header->payload_len = resp_payload_len;

      size_t resp_len = sizeof(RpcHeader) + resp_payload_len;
      auto send_view = rdmapp::mr_view(send_mr_, send_offset, resp_len);

      assert(resp_len <= kRecvBufferSize);

      try {
        co_await qp_->send(send_view, rdmapp::use_native_awaitable);
      } catch (const std::exception &e) {
        spdlog::error("server: send reply failed: {}", e.what());
      }

      // Send 完成，Buffer 现在是安全的，循环回到开头 Post Recv
    }
  }
};
