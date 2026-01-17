#pragma once
#include <atomic>
#include <coroutine>
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

// RPC 协议头
struct RpcHeader {
  uint64_t req_id;
  uint32_t payload_len;
  uint32_t padding; // 保持 8 字节对齐
};

// 状态槽，用于协调发送者和接收者
struct RpcSlot {
  std::atomic<bool> in_use{false};
  std::atomic<bool> done{false};
  std::atomic<std::coroutine_handle<>> waiter{};
  std::span<std::byte> user_resp_buffer; // 用户提供的接收地址
  size_t actual_len = 0;
};

// A simple RAII guard for std::atomic<bool>.
// Ensures the atomic is set to 'false' when the guard goes out of scope.
class AtomicGuard {
public:
  // Acquires the resource (points to the atomic).
  explicit AtomicGuard(std::atomic<bool> &flag) : flag_ptr_(&flag) {}

  // Destructor: Releases the resource.
  // This is the core of the RAII pattern.
  ~AtomicGuard() {
    if (flag_ptr_) {
      // Use 'release' memory order to synchronize with the 'acquire'
      // operation in the compare_exchange_weak loop. This ensures that
      // all memory operations before this point are visible to the next
      // thread that acquires the slot.
      flag_ptr_->store(false);
    }
  }

  // --- Rule of Five: Manage resource ownership correctly ---

  // 1. Delete copy constructor: Guards are unique owners; copying is a bug.
  AtomicGuard(const AtomicGuard &) = delete;

  // 2. Delete copy assignment:
  AtomicGuard &operator=(const AtomicGuard &) = delete;

  // 3. Move constructor: Transfer ownership from a temporary object.
  AtomicGuard(AtomicGuard &&other) noexcept : flag_ptr_(other.flag_ptr_) {
    // The old guard no longer owns the atomic flag.
    other.flag_ptr_ = nullptr;
  }

  // 4. Move assignment:
  AtomicGuard &operator=(AtomicGuard &&other) noexcept {
    if (this != &other) {
      // Release our current resource, if we own one.
      if (flag_ptr_) {
        flag_ptr_->store(false);
      }
      // Steal the resource from the other guard.
      flag_ptr_ = other.flag_ptr_;
      other.flag_ptr_ = nullptr;
    }
    return *this;
  }

private:
  std::atomic<bool> *flag_ptr_;
};

// 等待对象：挂起发送协程，直到接收 Worker 唤醒
struct RpcResponseAwaitable {
  RpcSlot &slot;
  bool await_ready() const noexcept { return slot.done.load(); }
  bool await_suspend(std::coroutine_handle<> h) noexcept {
    // 1. 尝试把自己的 handle 存进去
    // 使用 exchange 确保此时如果有人想 resume，他必须拿到这个 handle
    slot.waiter.store(h);

    // 2. 检查接收线程是否已经先于我完成了工作
    if (slot.done.load()) {
      // 3. 尝试把 handle 拿回来自己负责恢复（返回 false 即可）
      auto expected = h;
      if (slot.waiter.compare_exchange_strong(expected, nullptr)) {
        return false; // 拿回成功，告诉系统不要挂起，直接 resume
      }
      // 如果拿回来失败了，说明 Worker 线程已经把 handle 拿走并去 resume 了
      // 这种情况下，我们就不能返回 false，必须返回 true 让系统完成挂起
    }
    return true;
  }
  size_t await_resume() noexcept { return slot.actual_len; }
};

class RpcClient {
  static constexpr size_t kMaxInflight = 2048;

  std::shared_ptr<rdmapp::qp> qp_;

  // 发送内存池
  std::vector<std::byte> send_buffer_pool_;
  rdmapp::local_mr send_mr_;

  // 接收内存池
  std::vector<std::byte> recv_buffer_pool_;
  rdmapp::local_mr recv_mr_;

  std::vector<RpcSlot> slots_;
  std::atomic<uint64_t> req_seq_{0};

  cppcoro::static_thread_pool tp_{4};

  std::jthread worker_;

public:
  RpcClient(std::shared_ptr<rdmapp::qp> qp)
      : qp_(qp), send_buffer_pool_(kMaxInflight * kMsgSize),
        // 注册发送 MR
        send_mr_(qp->pd_ptr()->reg_mr(send_buffer_pool_.data(),
                                      send_buffer_pool_.size())),
        recv_buffer_pool_(kRecvDepth * kMsgSize),
        // 注册接收 MR
        recv_mr_(qp->pd_ptr()->reg_mr(recv_buffer_pool_.data(),
                                      recv_buffer_pool_.size())),
        slots_(kMaxInflight), worker_(&RpcClient::start_recv_workers, this) {
    spdlog::info("client: initialized with {} slots and {} recv workers",
                 kMaxInflight, kRecvDepth);
  }

  // 启动接收循环：产生 kRecvDepth 个并发的 Recv WR
  void start_recv_workers() {
    std::vector<cppcoro::task<void>> tasks;
    for (size_t i = 0; i < kRecvDepth; ++i) {
      tasks.emplace_back(cppcoro::schedule_on(tp_, recv_worker(i)));
    }
    cppcoro::sync_wait(cppcoro::when_all(std::move(tasks)));
  }

  // 发送 RPC 请求
  cppcoro::task<size_t> call(std::span<const std::byte> req_data,
                             std::span<std::byte> resp_buffer) {
    uint64_t seq = req_seq_.fetch_add(1);
    uint64_t slot_idx = seq % kMaxInflight;
    RpcSlot &slot = slots_[slot_idx];

    // 简单的忙等待分配策略（生产环境可用无锁队列优化）
    bool expected = false;
    int spin_cnt = 0;
    while (!slot.in_use.compare_exchange_weak(expected, true)) {
      expected = false;
      spin_cnt++;
      if (spin_cnt > 1000000) [[unlikely]] {
        spdlog::warn("fucking spinning: cnt={}", spin_cnt);
      }
    }
    AtomicGuard guard(slot.in_use);

    // 初始化 Slot
    slot.done.store(false);
    slot.user_resp_buffer = resp_buffer;
    slot.waiter = nullptr;

    // 准备发送数据：Header + Payload
    // 使用 mr_view 切片获取当前 Slot 对应的发送缓冲区
    size_t send_offset = slot_idx * kMsgSize;
    auto send_slice_mr = rdmapp::mr_view(send_mr_, send_offset,
                                         sizeof(RpcHeader) + req_data.size());

    RpcHeader *header =
        reinterpret_cast<RpcHeader *>(send_slice_mr.span().data());
    header->req_id = seq;
    header->payload_len = req_data.size();

    std::memcpy(send_slice_mr.span().data() + sizeof(RpcHeader),
                req_data.data(), req_data.size());

    // 发送请求 [1]
    try {
      co_await qp_->send(send_slice_mr, rdmapp::use_native_awaitable);
    } catch (const std::exception &e) {
      spdlog::error("client: send failed: {}", e.what());
      std::terminate();
      co_return 0;
    }

    // 等待响应
    size_t nbytes = co_await RpcResponseAwaitable{slot};

    co_return nbytes;
  }

private:
  // 单个接收 Worker：独占 recv_buffer_pool 的一部分
  cppcoro::task<void> recv_worker(size_t worker_idx) {
    spdlog::info("recv_worker[{}] started", worker_idx);

    size_t offset = worker_idx * kMsgSize;

    while (true) {
      // 创建对应此 Worker 的 MR 切片，用于接收 [1]
      // 注意：这里切片大小设为 kMsgSize，表示最大接收长度
      auto recv_slice_mr = rdmapp::mr_view(recv_mr_, offset, kMsgSize);

      try {
        // 投递 Recv WR 并挂起 [1]
        auto [nbytes, _] =
            co_await qp_->recv(recv_slice_mr, rdmapp::use_native_awaitable);

        if (nbytes < sizeof(RpcHeader)) {
          spdlog::warn("client: received packet too small: {}", nbytes);
          continue;
        }

        // 解析包头
        auto buffer_ptr = recv_buffer_pool_.data() + offset;
        auto *header = reinterpret_cast<RpcHeader *>(buffer_ptr);
        uint64_t req_id = header->req_id;
        uint32_t payload_len = header->payload_len;

        if (payload_len + sizeof(RpcHeader) > nbytes) {
          spdlog::error("client: payload len mismatch");
          continue;
        }

        // 查找 Slot
        uint64_t slot_idx = req_id % kMaxInflight;
        RpcSlot &slot = slots_[slot_idx];

        // 确认该 Slot 是活跃的
        if (slot.in_use.load()) {
          // 将数据直接 Copy 到用户的 Buffer
          size_t copy_len =
              std::min((size_t)payload_len, slot.user_resp_buffer.size());
          std::memcpy(slot.user_resp_buffer.data(),
                      buffer_ptr + sizeof(RpcHeader), copy_len);

          slot.actual_len = copy_len;

          slot.done.store(true);
          std::coroutine_handle<> h = slot.waiter.exchange(nullptr);
          if (h) {
            // 只有 exchange 成功拿到 handle 的人，才有资格执行 resume
            h.resume();
          }
        } else {
          spdlog::debug("client: recv unknown or expired req_id: {}", req_id);
        }

      } catch (const std::exception &e) {
        spdlog::error("client: recv worker error: {}", e.what());
        // 发生严重错误可能需要退出或重连，这里演示继续循环
        break;
      }
    }
  }
};

class RpcServer {
  std::shared_ptr<rdmapp::qp> qp_;
  cppcoro::static_thread_pool tp_{4};

  // 统一的大块内存池，避免频繁注册 MR
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
        spdlog::warn("server: recv too small packet");
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

      // 4. 发送响应
      // 计算实际发送大小
      size_t resp_len = sizeof(RpcHeader) + header->payload_len;
      auto send_view = rdmapp::mr_view(mr_, offset, resp_len);

      // 关键：必须等待 Send 完成！
      // 如果不 await，直接在此处 continue 回到 recv，
      // NIC 可能会同时在该 Buffer 上进行 DMA Read (Send) 和 DMA Write
      // (Recv)，导致数据损坏。
      try {
        co_await qp_->send(send_view, rdmapp::use_native_awaitable);
      } catch (const std::exception &e) {
        spdlog::error("server: send reply failed: {}", e.what());
      }

      // Send 完成，Buffer 现在是安全的，循环回到开头 Post Recv
    }
  }
};
