#pragma once

#include <type_traits>

namespace rdmapp {
/// @brief A tag type to request a native awaitable object return from an
/// asynchronous operation.
struct use_native_awaitable_t {};

/// @brief An instance of `use_native_awaitable_t` to be used as a completion
/// token.
inline constexpr auto use_native_awaitable = use_native_awaitable_t{};

/// @brief The default completion token used by asynchronous QP operations.
inline constexpr auto default_completion_token = use_native_awaitable;

/// @brief The default completion token type used by asynchronous QP operations.
using default_completion_token_t =
    std::remove_cvref_t<decltype(default_completion_token)>;

/**
 * @brief Concept to validate that a type is a valid completion token.
 */

template <typename T>
inline constexpr bool is_use_native_awaitable =
    std::is_same_v<std::remove_cvref_t<T>, use_native_awaitable_t>;

template <typename T>
concept ValidCompletionToken = is_use_native_awaitable<T>;

} // namespace rdmapp
