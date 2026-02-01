#include "rdmapp/executor.h"

#include "rdmapp/completion_token.h"
#include "rdmapp/detail/logger.h"
#include "rdmapp/qp.h"
#include <thread>

namespace rdmapp {

namespace executor_t {

template <typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
void execute_callback(struct ibv_wc const &wc) noexcept;

template <>
void execute_callback<use_native_awaitable_t>(
    struct ibv_wc const &wc) noexcept {
  basic_qp::operation_state *state =
      reinterpret_cast<basic_qp::operation_state *>(wc.wr_id);
  state->set_from_wc(wc);
  state->resume();
}

#ifdef RDMAPP_ASIO_COROUTINE
template <>
void execute_callback<use_asio_awaitable_t>(struct ibv_wc const &wc) noexcept {
#ifdef RDMAPP_BUILD_DEBUG
  auto thread_id = std::this_thread::get_id();
  log::trace("process_wc[thread={}]: {:#x}", thread_id, wc.wr_id);
#endif
  auto cb = reinterpret_cast<callback_ptr>(wc.wr_id);
  (*cb)(wc);
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("process_wc[thread={}]: done: {:#x}", thread_id, wc.wr_id);
#endif
  destroy_callback(cb);
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("process_wc[thread={}]: callback destroyed: {:#x}", thread_id,
             wc.wr_id);
#endif
}
#endif

void destroy_callback(callback_ptr cb) noexcept {
#ifdef RDMAPP_BUILD_DEBUG
  log::trace("executor: delete_callback: {}", log::fmt::ptr(cb));
#endif
  delete cb;
}
} // namespace executor_t

basic_executor::basic_executor() noexcept {}

template <typename CompletionToken>
requires ValidCompletionToken<CompletionToken>
void basic_executor::process_wc(std::span<struct ibv_wc> const wc) noexcept {
  for (auto const &w : wc) {
    executor_t::execute_callback<CompletionToken>(w);
  }
}

basic_executor::~basic_executor() noexcept {}

#ifdef RDMAPP_ASIO_COROUTINE
template void basic_executor::process_wc<use_asio_awaitable_t>(
    std::span<struct ibv_wc>) noexcept;
#endif
template void basic_executor::process_wc<use_native_awaitable_t>(
    std::span<struct ibv_wc>) noexcept;

} // namespace rdmapp
