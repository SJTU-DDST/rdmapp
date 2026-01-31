#pragma once

#include <type_traits>

namespace rdmapp {
#ifdef RDMAPP_ASIO_COROUTINE
/// @brief A tag type to request an `asio::awaitable` return from an
/// asynchronous operation.
struct use_asio_awaitable_t {};
#endif
/// @brief A tag type to request a native awaitable object return from an
/// asynchronous operation.
struct use_native_awaitable_t {};

#ifdef RDMAPP_ASIO_COROUTINE
/// @brief An instance of `use_asio_awaitable_t` to be used as a completion
/// token.
inline constexpr auto use_asio_awaitable = use_asio_awaitable_t{};
#endif
/// @brief An instance of `use_native_awaitable_t` to be used as a completion
/// token.
inline constexpr auto use_native_awaitable = use_native_awaitable_t{};

/// @brief The default completion token used by asynchronous QP operations.
#ifdef RDMAPP_ASIO_COROUTINE
inline constexpr auto default_completion_token = use_asio_awaitable;
#else
inline constexpr auto default_completion_token = use_native_awaitable;
#endif

/// @brief The default completion token type used by asynchronous QP operations.
using default_completion_token_t =
    std::remove_cvref_t<decltype(default_completion_token)>;

/**
 * @brief Concept to validate that a type is a valid completion token.
 */

#ifdef RDMAPP_ASIO_COROUTINE
template <typename T>
inline constexpr bool is_use_asio_awaitable =
    std::is_same_v<std::remove_cvref_t<T>, use_asio_awaitable_t>;
#endif

template <typename T>
inline constexpr bool is_use_native_awaitable =
    std::is_same_v<std::remove_cvref_t<T>, use_native_awaitable_t>;

template <typename T>
concept ValidCompletionToken =
#ifdef RDMAPP_ASIO_COROUTINE
    is_use_asio_awaitable<T> ||
#endif
    is_use_native_awaitable<T>;

} // namespace rdmapp
