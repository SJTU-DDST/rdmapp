#pragma once
#include <atomic>
#include <concurrentqueue.h> // moodycamel
#include <coroutine>
#include <cppcoro/async_mutex.hpp>
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

#include <rdmapp/qp.h>

inline constexpr size_t kMsgSize = 4096;
inline constexpr size_t kRecvDepth = 256;

struct RpcHeader {
  uint64_t req_id;
  uint32_t payload_len;
  uint32_t padding;
};

struct RpcSlot {
  std::atomic<bool> done{false};
  std::atomic<std::coroutine_handle<>> waiter{};
  std::span<std::byte> user_resp_buffer;
  size_t actual_len = 0;
  uint64_t expected_req_id = 0; // 关键：存储完整的编码后的 ID
};

struct RpcResponseAwaitable {
  RpcSlot &slot;
  bool await_ready() const noexcept { return slot.done.load(); }
  bool await_suspend(std::coroutine_handle<> h) noexcept {
    slot.waiter.store(h);
    if (!slot.done.load())
      return true;
    std::coroutine_handle<> expected = h;
    if (slot.waiter.compare_exchange_strong(expected, nullptr)) {
      return false;
    }
    return true;
  }
  size_t await_resume() noexcept { return slot.actual_len; }
};

class RpcClient {
  static constexpr size_t kMaxInflight = 256;
  std::shared_ptr<rdmapp::qp> qp_;

  std::vector<std::byte> send_buffer_pool_;
  rdmapp::local_mr send_mr_;
  std::vector<std::byte> recv_buffer_pool_;
  rdmapp::local_mr recv_mr_;

  std::vector<RpcSlot> slots_;

  moodycamel::ConcurrentQueue<uint32_t> free_slots_;

  std::atomic<uint64_t> global_seq_{0};
  cppcoro::static_thread_pool tp_{4};
  std::jthread worker_;

public:
  RpcClient(std::shared_ptr<rdmapp::qp> qp)
      : qp_(qp), send_buffer_pool_(kMaxInflight * kMsgSize),
        send_mr_(qp->pd_ptr()->reg_mr(send_buffer_pool_.data(),
                                      send_buffer_pool_.size())),
        recv_buffer_pool_(kRecvDepth * kMsgSize),
        recv_mr_(qp->pd_ptr()->reg_mr(recv_buffer_pool_.data(),
                                      recv_buffer_pool_.size())),
        slots_(kMaxInflight), free_slots_(kMaxInflight * 2),
        worker_(&RpcClient::start_recv_workers, this) {

    // 初始化空闲索引池
    for (uint32_t i = 0; i < kMaxInflight; ++i) {
      free_slots_.enqueue(i);
    }

    spdlog::info("client: initialized with {} slots", kMaxInflight);
  }

  void start_recv_workers() {
    std::vector<cppcoro::task<void>> tasks;
    for (size_t i = 0; i < kRecvDepth; ++i) {
      tasks.emplace_back(cppcoro::schedule_on(tp_, recv_worker(i)));
    }
    cppcoro::sync_wait(cppcoro::when_all(std::move(tasks)));
  }

  cppcoro::task<size_t> call(std::span<const std::byte> req_data,
                             std::span<std::byte> resp_buffer) {

    uint32_t slot_idx;
    while (!free_slots_.try_dequeue(slot_idx))
      ; // 信号量保证了这里一定能拿到

    // 2. 生成并编码 ID
    uint64_t seq = global_seq_.fetch_add(1);
    // 高 32 位 seq，低 32 位 slot_idx
    uint64_t req_id = (seq << 32) | static_cast<uint64_t>(slot_idx);

    RpcSlot &slot = slots_[slot_idx];
    slot.done.store(false);
    slot.user_resp_buffer = resp_buffer;
    slot.waiter = nullptr;
    slot.expected_req_id = req_id;

    // 3. 准备数据并发送
    size_t send_offset = slot_idx * kMsgSize;
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
    size_t offset = worker_idx * kMsgSize;
    while (true) {
      auto recv_slice_mr = rdmapp::mr_view(recv_mr_, offset, kMsgSize);
      try {
        auto [nbytes, _] =
            co_await qp_->recv(recv_slice_mr, rdmapp::use_native_awaitable);

        if (nbytes < sizeof(RpcHeader))
          continue;

        auto buffer_ptr = recv_buffer_pool_.data() + offset;
        auto header = reinterpret_cast<RpcHeader *>(buffer_ptr);

        uint64_t recv_id = header->req_id;
        // 解码：取低 32 位作为索引
        uint32_t slot_idx = static_cast<uint32_t>(recv_id & 0xFFFFFFFF);

        if (slot_idx >= kMaxInflight) {
          spdlog::error("client: invalid slot_idx decoded: {}", slot_idx);
          continue;
        }

        RpcSlot &slot = slots_[slot_idx];

        // 校验：ID 必须完全匹配 且 Slot 确实在等待
        if (!slot.done.load() && slot.expected_req_id == recv_id) {
          // 拷贝数据
          size_t payload_len = header->payload_len;
          size_t copy_len =
              std::min((size_t)payload_len, slot.user_resp_buffer.size());
          std::memcpy(slot.user_resp_buffer.data(),
                      buffer_ptr + sizeof(RpcHeader), copy_len);

          slot.actual_len = copy_len;
          slot.done.store(true);

          std::coroutine_handle<> h = slot.waiter.exchange(nullptr);
          if (h) {
            h.resume();
          }
        } else {
          spdlog::warn(
              "client: received late or mismatch packet for req_id: {}",
              recv_id);
        }
      } catch (const std::exception &e) {
        spdlog::error("client: recv worker error: {}", e.what());
        break;
      }
    }
  }
};

class RpcServer {
  std::shared_ptr<rdmapp::qp> qp_;
  cppcoro::static_thread_pool tp_{4};

  std::vector<std::byte> buffer_pool_;
  rdmapp::local_mr mr_;

public:
  RpcServer(std::shared_ptr<rdmapp::qp> qp)
      : qp_(qp), buffer_pool_(kRecvDepth * kMsgSize),
        mr_(qp->pd_ptr()->reg_mr(buffer_pool_.data(), buffer_pool_.size())) {
    spdlog::info("server: initialized with {} concurrent workers", kRecvDepth);
  }

  // 入口：启动所有 Worker 并保持运行
  cppcoro::task<void> run() {
    std::vector<cppcoro::task<void>> workers;
    workers.reserve(kRecvDepth);

    // 启动 N 个并发循环，瞬间填满 RQ
    for (size_t i = 0; i < kRecvDepth; ++i) {
      workers.emplace_back(server_worker(i));
    }

    // 等待所有协程（实际上是无限循环）
    co_await cppcoro::when_all(std::move(workers));
  }

private:
  // 单个 Worker：负责一个 Slot 的 接收 -> 处理 -> 发送 循环
  cppcoro::task<void> server_worker(size_t idx) {
    size_t offset = idx * kMsgSize;
    spdlog::info("server_worker {} spawn", idx);

    while (true) {
      // 1. 准备接收视口 (View)
      // 使用 mr_view 构造切片，避免重复注册 [1]
      auto recv_view = rdmapp::mr_view(mr_, offset, kMsgSize);

      // 2. 挂起等待请求
      // 此时 Buffer 归 NIC 写入，CPU 不可动
      auto [nbytes, _] =
          co_await qp_->recv(recv_view, rdmapp::use_native_awaitable);

      if (nbytes < sizeof(RpcHeader)) {
        spdlog::warn("server: recv too small packet: size={} expected={}",
                     nbytes, sizeof(RpcHeader));
        continue; // 重新投递 Recv
      }

      // 3. 调度到业务线程池
      // 此时数据已在 Buffer，切换到 CPU 密集型线程处理
      co_await tp_.schedule();

      // ---------------- 业务逻辑区域 ----------------
      auto *header =
          reinterpret_cast<RpcHeader *>(buffer_pool_.data() + offset);

      // 简单的校验
      if (header->payload_len + sizeof(RpcHeader) > nbytes) {
        spdlog::error("server: header payload len mismatch");
        // 可以在这里 continue，但需要考虑是否需要回复错误包
        // 为简化，这里假设数据总是合法的，继续处理
      }

      std::span<std::byte> payload{buffer_pool_.data() + offset +
                                       sizeof(RpcHeader),
                                   header->payload_len};

      // 模拟业务处理：原地修改 Payload
      std::fill(payload.begin(), payload.end(), std::byte{0xEE});

      // 更新 Header：Response 长度 (ReqID 保持不变)
      header->payload_len = payload.size();
      // ---------------------------------------------

      size_t resp_len = sizeof(RpcHeader) + header->payload_len;
      auto send_view = rdmapp::mr_view(mr_, offset, resp_len);

      try {
        co_await qp_->send(send_view, rdmapp::use_native_awaitable);
      } catch (const std::exception &e) {
        spdlog::error("server: send reply failed: {}", e.what());
      }

      // Send 完成，Buffer 现在是安全的，循环回到开头 Post Recv
    }
  }
};
