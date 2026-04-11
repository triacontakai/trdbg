#ifndef BACKENDS_BACKEND_H_
#define BACKENDS_BACKEND_H_

#include <concepts>
#include <cstddef>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>

namespace tdb::backends {

enum class RegisterError {
    RegisterDoesNotExist,
};

/**
 * Helper class for backend errors
 * main purpose is just to provide error messages to users
 */
class BackendError {
public:
    BackendError(std::error_code err) : err_(err) {};
    BackendError(std::string message) : message_(std::move(message)) {};

    std::string message() const {
        if (err_)
            return err_.message();
        else
            return message_;
    }
private:
    std::error_code err_;
    std::string message_;
};

/**
 * Why the process stopped, returned from wait()
 * code is the signal number for Stopped/Killed, and exit status for Exited
 */
struct StopEvent {
    enum class Reason {
        Stopped,
        Exited,
        Killed,
    };

    Reason reason;
    int code;
};

template<typename T>
concept RegisterSet =
    std::ranges::forward_range<T> &&
    std::same_as<
        std::ranges::range_value_t<T>,
        std::tuple<std::string_view, std::reference_wrapper<typename T::max_register_size>>
    > &&
    requires(T& x, std::string_view reg_name, T::max_register_size val) {
        { x.get_register(reg_name) } -> std::same_as<std::expected<typename T::max_register_size, RegisterError>>;
        { x.set_register(reg_name, val) } -> std::same_as<std::expected<void, RegisterError>>;
        { x.register_size(reg_name) } -> std::same_as<std::expected<std::size_t, RegisterError>>;
    };

template<typename T>
concept Backend = RegisterSet<typename T::registers> && requires(
        T& x,
        const typename T::registers& registers,
        std::string_view path,
        std::span<const std::string> args,
        int pid) {
    { x.launch(path, args) } -> std::same_as<std::expected<void, BackendError>>;
    { x.attach(pid) } -> std::same_as<std::expected<void, BackendError>>;

    { x.resume() } -> std::same_as<std::expected<void, BackendError>>;
    { x.step() } -> std::same_as<std::expected<void, BackendError>>;
    { x.wait() } -> std::same_as<std::expected<StopEvent, BackendError>>;
    { x.interrupt() } -> std::same_as<std::expected<void, BackendError>>;
    { x.kill() } -> std::same_as<std::expected<void, BackendError>>;

    { x.get_registers() } -> std::same_as<std::expected<typename T::registers, BackendError>>;
    { x.set_registers(registers) } -> std::same_as<std::expected<void, BackendError>>;
};

template<typename T>
concept LinuxBackend = Backend<T> &&
    requires (T& x, int signal) {
        { x.send_signal(signal) } -> std::same_as<std::expected<void, BackendError>>;
    };

} // tdb::backends

#endif
