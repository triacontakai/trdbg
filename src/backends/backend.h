#ifndef BACKENDS_BACKENDS_H_
#define BACKENDS_BACKENDS_H_

#include <concepts>
#include <expected>
#include <ranges>
#include <variant>
#include <string_view>
#include <system_error>
#include <utility>

namespace tdb::backends {

enum class RegisterError {
    ValueOutOfBounds,
};

template<typename T>
concept RegisterSet =
    std::ranges::input_range<T> &&
    std::same_as<
        std::ranges::range_value_t<T>,
        std::tuple<std::string_view, typename T::max_register_size&>
    > &&
    requires(T& x, std::string_view reg_name, T::max_register_size val) {
        { x.get_register(reg_name) } -> std::same_as<typename T::max_register_size>;
        { x.set_register(reg_name, val) } -> std::same_as<std::expected<std::monostate, RegisterError>>;
        { x.register_size(reg_name) } -> std::same_as<size_t>;
    };

template<typename T>
concept Backend = requires(T& x, T::register_set& registers, int signal) {
    { x.run() } -> std::same_as<std::error_code>;

    { x.start() } -> std::same_as<void>;
    { x.stop() } -> std::same_as<void>;
    { x.terminate() } -> std::same_as<std::error_code>;
    { x.kill() } -> std::same_as<std::error_code>;

    { x.send_signal(signal) } -> std::same_as<std::error_code>;
    { x.get_registers(registers) } -> std::same_as<void>;
};

}

#endif
