#ifndef BACKENDS_BACKEND_H_
#define BACKENDS_BACKEND_H_

#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include "event_loop.h"

namespace tdb::backends {

enum class RegisterError {
    RegisterDoesNotExist,
};

/**
 * helper class for backend errors
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
 * why the process stopped, passed to the on_event() handler
 * code is the signal number for Stopped/Killed, exit status for Exited, and breakpoint id for Breakpoint
 */
struct StopEvent {
    enum class Reason {
        Stopped,
        Breakpoint,
        Exited,
        Killed,
    };

    Reason reason;
    int code;
};

enum class ProcessState {
    None,
    Running,
    Stopped,
};

/**
 * Something the user can refer to in memory, e.g. an address (later symbols, $reg+offset, etc.)
 * comes from Backend::parse_location, and Backend::resolve turns it into an address
 * kept separate from addresses since some locations can only be resolved once the process is running
 */
template<typename T>
concept Location = std::copyable<T> && std::formattable<T, char>;

using BreakpointId = unsigned int;

template<Location L, std::unsigned_integral A>
struct Breakpoint {
    BreakpointId id;
    L location;
    // where it's inserted right now, empty while it's pending (no process, or it couldn't be resolved/written)
    std::optional<A> address;
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
        { x.pc() } -> std::same_as<typename T::max_register_size>;
    };

template<typename T>
concept Backend =
    RegisterSet<typename T::registers> &&
    Location<typename T::location> &&
    std::unsigned_integral<typename T::address> &&
    std::constructible_from<T, EventLoop&> &&
    requires(
        T& x,
        const typename T::registers& registers,
        std::string_view location_str,
        const typename T::location& location,
        typename T::address address,
        std::span<std::byte> out,
        std::span<const std::byte> in,
        BreakpointId id,
        std::string_view path,
        std::span<const std::string> args,
        int pid,
        std::function<void(StopEvent)> handler) {
    { x.launch(path, args) } -> std::same_as<std::expected<void, BackendError>>;
    { x.attach(pid) } -> std::same_as<std::expected<void, BackendError>>;

    { x.resume() } -> std::same_as<std::expected<void, BackendError>>;
    { x.step() } -> std::same_as<std::expected<void, BackendError>>;
    { x.interrupt() } -> std::same_as<std::expected<void, BackendError>>;
    { x.kill() } -> std::same_as<std::expected<void, BackendError>>;

    { x.on_event(handler) } -> std::same_as<void>;
    { x.state() } -> std::same_as<ProcessState>;

    { x.get_registers() } -> std::same_as<std::expected<typename T::registers, BackendError>>;
    { x.set_registers(registers) } -> std::same_as<std::expected<void, BackendError>>;

    { x.parse_location(location_str) } -> std::same_as<std::expected<typename T::location, BackendError>>;
    { x.resolve(location) } -> std::same_as<std::expected<typename T::address, BackendError>>;
    { x.read_memory(address, out) } -> std::same_as<std::expected<void, BackendError>>;
    { x.write_memory(address, in) } -> std::same_as<std::expected<void, BackendError>>;

    { x.add_breakpoint(location) } -> std::same_as<std::expected<BreakpointId, BackendError>>;
    { x.remove_breakpoint(id) } -> std::same_as<std::expected<void, BackendError>>;
    { x.breakpoints() } -> std::same_as<std::vector<Breakpoint<typename T::location, typename T::address>>>;
};

template<typename T>
concept LinuxBackend = Backend<T> &&
    requires (T& x, int signal) {
        { x.send_signal(signal) } -> std::same_as<std::expected<void, BackendError>>;
    };

} // tdb::backends

#endif
