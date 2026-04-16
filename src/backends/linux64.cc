#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <system_error>
#include <sys/ptrace.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include "linux64.h"
#include "backend.h"

#define REG_INFO(reg) std::make_pair(std::string_view(#reg), offsetof(struct user_regs_struct, reg))

#define ERROR_NOT_RUNNING std::string("process not started")
#define ERROR_NOT_STOPPED std::string("process not stopped")

namespace tdb::backends {

static_assert(RegisterSet<Amd64Registers>);
static_assert(LinuxBackend<Linux64Backend>);

namespace {
// defined display/iteration order of registers
// this allows some semantic grouping that isn't just the order they are defined in the struct
constexpr std::array REGISTER_ORDER = {
    REG_INFO(rax),
    REG_INFO(rbx),
    REG_INFO(rcx),
    REG_INFO(rdx),
    REG_INFO(rsi),
    REG_INFO(rdi),
    REG_INFO(rbp),
    REG_INFO(rsp),
    REG_INFO(r8),
    REG_INFO(r9),
    REG_INFO(r10),
    REG_INFO(r11),
    REG_INFO(r12),
    REG_INFO(r13),
    REG_INFO(r14),
    REG_INFO(r15),
    REG_INFO(rip),
    REG_INFO(eflags),
    REG_INFO(cs),
    REG_INFO(ss),
    REG_INFO(ds),
    REG_INFO(es),
    REG_INFO(fs),
    REG_INFO(gs),
    REG_INFO(fs_base),
    REG_INFO(gs_base),
};

// unordered_map for faster lookup of registers than using REGISTER_ORDER
const std::unordered_map<std::string_view, std::size_t> REGISTER_OFFSETS(
    REGISTER_ORDER.begin(), REGISTER_ORDER.end());

template<typename T>
concept Pointer = std::is_pointer_v<T>;

// gets offset
template<Pointer P, std::integral O>
constexpr Amd64Registers::max_register_size& reg_at_offset(P ptr, O offset) {
    auto *base = reinterpret_cast<char *>(ptr);
    return *reinterpret_cast<Amd64Registers::max_register_size *>(base + offset);
}

std::unexpected<BackendError> errno_error(int err = errno) {
    return std::unexpected(BackendError(std::error_code(err, std::system_category())));
}
} // anonymous namespace

auto Amd64Registers::Iterator::operator*() const -> value_type {
    auto [reg_name, reg_offset] = REGISTER_ORDER[index_];
    return std::make_tuple(reg_name, std::ref(reg_at_offset(&parent_->regs_, reg_offset)));
}

auto Amd64Registers::Iterator::operator++() -> Iterator& {
    index_++;
    return *this;
}

auto Amd64Registers::Iterator::operator++(int) -> Iterator {
    auto old = *this;
    ++*this;
    return old;
}

auto Amd64Registers::get_register(std::string_view reg) -> std::expected<max_register_size, RegisterError> {
    if (auto found = REGISTER_OFFSETS.find(reg); found != REGISTER_OFFSETS.end())
        return reg_at_offset(&regs_, found->second);
    else
        return std::unexpected(RegisterError::RegisterDoesNotExist);
}

std::expected<void, RegisterError> Amd64Registers::set_register(std::string_view reg, max_register_size value) {
    if (auto found = REGISTER_OFFSETS.find(reg); found != REGISTER_OFFSETS.end()) {
        reg_at_offset(&regs_, found->second) = value;
        return {};
    } else {
        return std::unexpected(RegisterError::RegisterDoesNotExist);
    }
}

std::expected<std::size_t, RegisterError> Amd64Registers::register_size(std::string_view reg) {
    // all registers in user_regs_struct are the same size
    if (REGISTER_OFFSETS.contains(reg))
        return sizeof(max_register_size);
    else
        return std::unexpected(RegisterError::RegisterDoesNotExist);
}

auto Amd64Registers::begin() -> Iterator {
    return Iterator(this, 0);
}

auto Amd64Registers::end() -> Iterator {
    return Iterator(this, REGISTER_ORDER.size());
}

Linux64Backend::~Linux64Backend() {
    // don't leave a traced process behind
    if (pid_ != 0 && ::kill(pid_, SIGKILL) == 0) {
        while (pid_ != 0 && wait())
            ;
    }

    if (signal_fd_ != -1) {
        loop_.unwatch(signal_fd_watch_);
        close(signal_fd_);
    }
}

std::expected<void, BackendError> Linux64Backend::launch(std::string_view path, std::span<const std::string> args) {
    // if we are already running, disallow running again
    if (pid_ != 0)
        return std::unexpected(std::string("process already running"));

    // build argv before fork so the child doesn't need to allocate
    std::string path_str(path);
    std::vector<char *> argv;
    argv.push_back(path_str.data());
    for (auto const& arg : args)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);

    if (auto ok = setup_child_events(); !ok)
        return ok;

    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) == -1)
        return errno_error();

    pid_t pid = fork();
    if (pid == -1) {
        int err = errno;
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return errno_error(err);
    }

    if (pid == 0) {
        // child
        close(pipe_fds[0]);

        // signal mask survives execve, so put back the one from before we blocked SIGCHLD
        sigprocmask(SIG_SETMASK, &original_mask_, nullptr);

        if (ptrace(PTRACE_TRACEME) == 0)
            execvp(argv[0], argv.data());

        // getting here means ptrace/execve failed, so send error through pipe
        int err = errno;
        write(pipe_fds[1], &err, sizeof(err));
        _exit(EXIT_FAILURE);
    }

    // closing write end here means child owns the only remaining writer
    // if child execve succeeds, O_CLOEXEC means that writer is closed
    // this results in read returning 0 in success case, and err on fail
    close(pipe_fds[1]);
    int err;
    ssize_t bytes_read = read(pipe_fds[0], &err, sizeof(err));
    close(pipe_fds[0]);
    if (bytes_read > 0) {
        // reap child so it doesn't stick around as a zombie
        waitpid(pid, nullptr, 0);
        return errno_error(err);
    }

    pid_ = pid;

    // child stops with SIGTRAP after execve, consume that stop
    if (auto event = wait(); !event)
        return std::unexpected(event.error());

    return {};
}

std::expected<void, BackendError> Linux64Backend::attach(int pid) {
    return std::unexpected(std::string("attach not implemented"));
}

std::expected<void, BackendError> Linux64Backend::resume() {
    if (auto ok = check_stopped(); !ok)
        return ok;

    if (ptrace(PTRACE_CONT, pid_, 0, pending_signal_) == -1)
        return errno_error();

    stopped_ = false;
    pending_signal_ = 0;
    return {};
}

std::expected<void, BackendError> Linux64Backend::step() {
    if (auto ok = check_stopped(); !ok)
        return ok;

    if (ptrace(PTRACE_SINGLESTEP, pid_, 0, pending_signal_) == -1)
        return errno_error();

    stopped_ = false;
    pending_signal_ = 0;
    return {};
}

std::expected<void, BackendError> Linux64Backend::interrupt() {
    return send_signal(SIGSTOP);
}

std::expected<void, BackendError> Linux64Backend::kill() {
    if (pid_ == 0)
        return std::unexpected(ERROR_NOT_RUNNING);

    if (::kill(pid_, SIGKILL) == -1)
        return errno_error();

    stopped_ = false;
    return {};
}

void Linux64Backend::on_event(std::function<void(StopEvent)> handler) {
    event_handler_ = std::move(handler);
}

auto Linux64Backend::get_registers() -> std::expected<registers, BackendError> {
    if (auto ok = check_stopped(); !ok)
        return std::unexpected(ok.error());

    registers regs;
    if (ptrace(PTRACE_GETREGS, pid_, 0, &regs.regs_) == -1)
        return errno_error();

    return regs;
}

std::expected<void, BackendError> Linux64Backend::set_registers(const registers& regs) {
    if (auto ok = check_stopped(); !ok)
        return ok;

    if (ptrace(PTRACE_SETREGS, pid_, 0, &regs.regs_) == -1)
        return errno_error();

    return {};
}

std::expected<void, BackendError> Linux64Backend::send_signal(int signal) {
    if (pid_ == 0)
        return std::unexpected(ERROR_NOT_RUNNING);

    if (::kill(pid_, signal) == -1)
        return errno_error();

    return {};
}

void Linux64Backend::handle_signal_fd() {
    // drain before waitpid, so a SIGCHLD that shows up in between leaves the fd readable
    // SIGCHLDs get merged anyway so we don't care what's in them
    signalfd_siginfo info;
    while (read(signal_fd_, &info, sizeof(info)) > 0) {}

    // the handler might resume the process, so keep going until nothing is left
    int status;
    while (pid_ != 0 && waitpid(pid_, &status, WNOHANG) > 0) {
        StopEvent event = handle_status(status);
        if (event_handler_)
            event_handler_(event);
    }
}

// blocking wait that skips the event loop, for when we need the result right away
std::expected<StopEvent, BackendError> Linux64Backend::wait() {
    int status;
    int ret;
    do {
        ret = waitpid(pid_, &status, 0);
    } while (ret == -1 && errno == EINTR);

    if (ret == -1)
        return errno_error();

    return handle_status(status);
}

StopEvent Linux64Backend::handle_status(int status) {
    if (WIFEXITED(status)) {
        pid_ = 0;
        stopped_ = false;
        return StopEvent{StopEvent::Reason::Exited, WEXITSTATUS(status)};
    }

    if (WIFSIGNALED(status)) {
        pid_ = 0;
        stopped_ = false;
        return StopEvent{StopEvent::Reason::Killed, WTERMSIG(status)};
    }

    // SIGTRAP and SIGSTOP are from us, anything else should be passed on to the process
    int signal = WSTOPSIG(status);
    if (signal != SIGTRAP && signal != SIGSTOP)
        pending_signal_ = signal;

    stopped_ = true;
    return StopEvent{StopEvent::Reason::Stopped, signal};
}

std::expected<void, BackendError> Linux64Backend::setup_child_events() {
    if (signal_fd_ != -1)
        return {};

    // SIGCHLD has to be blocked for signalfd to receive it
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (int err = pthread_sigmask(SIG_BLOCK, &mask, &original_mask_); err != 0)
        return errno_error(err);

    signal_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ == -1) {
        int err = errno;
        pthread_sigmask(SIG_SETMASK, &original_mask_, nullptr);
        return errno_error(err);
    }

    signal_fd_watch_ = loop_.watch_fd(signal_fd_, [this] { handle_signal_fd(); });
    return {};
}

std::expected<void, BackendError> Linux64Backend::check_stopped() const {
    if (pid_ == 0)
        return std::unexpected(ERROR_NOT_RUNNING);
    if (!stopped_)
        return std::unexpected(ERROR_NOT_STOPPED);
    return {};
}

} // tdb::backends
