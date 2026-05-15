#ifndef BACKENDS_LINUX_H_
#define BACKENDS_LINUX_H_

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>
#include <sys/user.h>

#include "backend.h"
#include "event_loop.h"

namespace tdb::backends {

class Amd64Registers {
public:
    // same type as the fields in user_regs_struct so references into it are valid
    using max_register_size = decltype(user_regs_struct::rax);
    class Iterator {
    public:
        using value_type = std::tuple<std::string_view, std::reference_wrapper<max_register_size>>;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::forward_iterator_tag;

        value_type operator*() const;

        Iterator& operator++();
        Iterator operator++(int);
        bool operator==(Iterator const& other) const = default;

        Iterator() = default;
    private:
        explicit Iterator(Amd64Registers *parent, std::size_t index)
            : parent_(parent), index_(index) {};

        Amd64Registers *parent_ = nullptr;
        std::size_t index_ = 0;

        friend class Amd64Registers;
    };

    std::expected<max_register_size, RegisterError> get_register(std::string_view reg);
    std::expected<void, RegisterError> set_register(std::string_view reg, max_register_size value);
    std::expected<std::size_t, RegisterError> register_size(std::string_view reg_name);
    max_register_size pc() const;

    Iterator begin();
    Iterator end();

private:
    struct user_regs_struct regs_ {};

    friend class Linux64Backend;
};

// only raw addresses (written *addr) for now, symbols etc. will go here later
struct Linux64Location {
    std::uint64_t address;
};

}

// has to come before anything that needs Location<Linux64Location>, like Breakpoint
template<>
struct std::formatter<tdb::backends::Linux64Location> : std::formatter<std::string> {
    auto format(const tdb::backends::Linux64Location& loc, auto& ctx) const {
        return std::formatter<std::string>::format(std::format("*{:#x}", loc.address), ctx);
    }
};

namespace tdb::backends {

class Linux64Backend {
public:
    using registers = Amd64Registers;
    using location = Linux64Location;
    using address = std::uint64_t;
    using breakpoint = Breakpoint<location, address>;

    explicit Linux64Backend(EventLoop& loop) : loop_(loop) {};
    Linux64Backend(const Linux64Backend&) = delete;
    Linux64Backend& operator=(const Linux64Backend&) = delete;
    ~Linux64Backend();

    std::expected<void, BackendError> launch(std::string_view path, std::span<const std::string> args);
    std::expected<void, BackendError> attach(int pid);

    std::expected<void, BackendError> resume();
    std::expected<void, BackendError> step();
    std::expected<void, BackendError> interrupt();
    // the exit shows up as a Killed event like any other
    std::expected<void, BackendError> kill();

    // handler runs on the event loop thread whenever the process stops/exits/gets killed
    // SIGCHLD gets blocked on the thread that calls launch(), so do that before spawning other threads
    void on_event(std::function<void(StopEvent)> handler);
    ProcessState state() const;

    std::expected<registers, BackendError> get_registers();
    std::expected<void, BackendError> set_registers(const registers& regs);

    std::expected<location, BackendError> parse_location(std::string_view str);
    std::expected<address, BackendError> resolve(const location& loc);
    std::expected<void, BackendError> read_memory(address addr, std::span<std::byte> out);
    std::expected<void, BackendError> write_memory(address addr, std::span<const std::byte> in);

    // with no process the breakpoint is pending, and gets inserted on the next launch
    std::expected<BreakpointId, BackendError> add_breakpoint(const location& loc);
    std::expected<void, BackendError> remove_breakpoint(BreakpointId id);
    std::vector<breakpoint> breakpoints() const;

    std::expected<void, BackendError> send_signal(int signal);

private:
    std::expected<void, BackendError> check_stopped() const;
    std::expected<void, BackendError> setup_child_events();
    void handle_signal_fd();
    std::expected<StopEvent, BackendError> wait();
    std::expected<void, BackendError> access_memory(address addr, std::byte *buf, std::size_t size, bool write);
    std::expected<void, BackendError> poke_byte(address addr, std::byte value);
    std::optional<StopEvent> handle_status(int status);
    void forget_process();

    std::expected<void, BackendError> insert_int3(address addr);
    std::expected<void, BackendError> remove_int3(address addr);
    bool at_breakpoint();
    std::expected<void, BackendError> start_step_over(bool then_continue);
    std::optional<address> trapped_breakpoint();
    std::expected<void, BackendError> set_pc(address addr);

    EventLoop& loop_;
    std::function<void(StopEvent)> event_handler_;

    pid_t pid_ = 0;
    bool stopped_ = false;

    // signal to deliver on next resume/step
    int pending_signal_ = 0;

    int signal_fd_ = -1;
    EventLoop::Handle signal_fd_watch_;

    std::vector<breakpoint> breakpoints_;
    BreakpointId next_breakpoint_id_ = 1;
    // what was under each int3 we've written, keyed by address (so the keys are also where the int3s are)
    std::map<address, std::byte> original_bytes_;
    // set while single stepping off a breakpoint, so its int3 can go back in once the step is done
    std::optional<address> step_over_;
    bool continue_after_step_over_ = false;
};

}

#endif
