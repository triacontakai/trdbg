#ifndef BACKENDS_LINUX_H_
#define BACKENDS_LINUX_H_

#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <sys/user.h>

#include "backend.h"

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

    std::expected<void, BackendError> launch(std::string_view path, std::span<const std::string> args);
    std::expected<void, BackendError> attach(int pid);

    std::expected<void, BackendError> resume();
    std::expected<void, BackendError> step();
    std::expected<StopEvent, BackendError> wait();
    std::expected<void, BackendError> interrupt();
    std::expected<void, BackendError> kill();

    std::expected<registers, BackendError> get_registers();
    std::expected<void, BackendError> set_registers(const registers& regs);

    std::expected<void, BackendError> send_signal(int signal);

private:
    std::expected<void, BackendError> ensure_stopped() const;

    pid_t pid_ = 0;
    bool stopped_ = false;
    // signal to deliver on next resume/step
    int pending_signal_ = 0;
};

}

#endif
