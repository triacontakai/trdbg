#ifndef BACKENDS_LINUX_H_
#define BACKENDS_LINUX_H_

#include <cstddef>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <signal.h>
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

    Iterator begin();
    Iterator end();

private:
    struct user_regs_struct regs_ {};

    friend class Linux64Backend;
};

class Linux64Backend {
public:
    using registers = Amd64Registers;

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

    std::expected<registers, BackendError> get_registers();
    std::expected<void, BackendError> set_registers(const registers& regs);

    std::expected<void, BackendError> send_signal(int signal);

private:
    std::expected<void, BackendError> check_stopped() const;
    std::expected<void, BackendError> setup_child_events();
    void handle_signal_fd();
    std::expected<StopEvent, BackendError> wait();
    StopEvent handle_status(int status);

    EventLoop& loop_;
    std::function<void(StopEvent)> event_handler_;

    pid_t pid_ = 0;
    bool stopped_ = false;
    // signal to deliver on next resume/step
    int pending_signal_ = 0;

    int signal_fd_ = -1;
    EventLoop::Handle signal_fd_watch_;
    // signal mask from before we blocked SIGCHLD, restored in the child
    sigset_t original_mask_;
};

}

#endif
