#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <elf.h>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <system_error>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include "linux64.h"
#include "backend.h"
#include "parse.h"

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

constexpr std::byte INT3{0xcc};

std::string_view trim(std::string_view s) {
    auto start = s.find_first_not_of(' ');
    if (start == std::string_view::npos)
        return {};
    return s.substr(start, s.find_last_not_of(' ') - start + 1);
}

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

auto Amd64Registers::pc() const -> max_register_size {
    return regs_.rip;
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

        // signal mask survives execve, and we (and the frontend) block signals for signalfd
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, nullptr);

        // disables ASLR so addresses (and breakpoints on them) stay the same between runs
        personality(personality(0xffffffff) | ADDR_NO_RANDOMIZE);

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

    // symbols get reread in case the program was rebuilt
    // PIE executables require the runtime base address
    // we ignore error here since this just means no available symbols
    std::ignore = load_executable(path);

    // breakpoints from before the process existed (or from the last run) go in now
    for (auto& bp : breakpoints_) {
        if (auto addr = resolve(bp.location); addr && insert_int3(*addr))
            bp.address = *addr;
    }

    return {};
}

std::expected<void, BackendError> Linux64Backend::attach(int pid) {
    return std::unexpected(std::string("attach not implemented"));
}

std::expected<void, BackendError> Linux64Backend::resume() {
    if (auto ok = check_stopped(); !ok)
        return ok;

    // can't run straight through an int3 we put at pc, step off it first
    if (at_breakpoint())
        return start_step_over(true);

    if (ptrace(PTRACE_CONT, pid_, 0, pending_signal_) == -1)
        return errno_error();

    stopped_ = false;
    pending_signal_ = 0;
    return {};
}

std::expected<void, BackendError> Linux64Backend::step() {
    if (auto ok = check_stopped(); !ok)
        return ok;

    if (at_breakpoint())
        return start_step_over(false);

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

ProcessState Linux64Backend::state() const {
    if (pid_ == 0)
        return ProcessState::None;
    if (stopped_)
        return ProcessState::Stopped;
    return ProcessState::Running;
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

auto Linux64Backend::parse_location(std::string_view str) -> std::expected<location, BackendError> {
    // addresses need * - bare words are left for symbols/line numbers later
    if (str.starts_with('*')) {
        if (auto addr = parse_integer<address>(trim(str.substr(1))))
            return location{location::Address{*addr}};
        return std::unexpected(std::format("invalid location `{}'", str));
    }

    if (parse_integer<address>(str))
        return std::unexpected(std::format("invalid location `{}' (addresses are written *{})", str, str));

    // anything else is a symbol, optionally with +offset
    std::string_view name = str;
    address offset = 0;
    if (auto plus = str.find('+'); plus != std::string_view::npos) {
        name = trim(str.substr(0, plus));
        auto parsed = parse_integer<address>(trim(str.substr(plus + 1)));
        if (!parsed)
            return std::unexpected(std::format("invalid offset in `{}'", str));
        offset = *parsed;
    }

    if (name.empty())
        return std::unexpected(std::format("invalid location `{}'", str));
    if (!symbols_)
        return std::unexpected(std::format("no symbol `{}' (no symbols loaded)", name));
    if (!symbols_->find(name))
        return std::unexpected(std::format("no symbol `{}'", name));

    return location{location::Symbol{std::string(name), offset}};
}

auto Linux64Backend::resolve(const location& loc) -> std::expected<address, BackendError> {
    if (auto addr = std::get_if<location::Address>(&loc.value))
        return addr->address;

    auto const& sym = std::get<location::Symbol>(loc.value);
    auto found = symbols_ ? symbols_->find(sym.name) : std::nullopt;
    if (!found)
        return std::unexpected(std::format("no symbol `{}'", sym.name));
    if (!load_offset_)
        return std::unexpected(std::format("can't resolve `{}' until the program is running", sym.name));

    return *load_offset_ + found->value + sym.offset;
}

std::expected<void, BackendError> Linux64Backend::read_memory(address addr, std::span<std::byte> out) {
    if (auto ok = access_memory(addr, out.data(), out.size(), false); !ok)
        return ok;

    // show what's really there instead of our int3s
    auto first = original_bytes_.lower_bound(addr);
    auto last = original_bytes_.lower_bound(addr + out.size());
    for (auto it = first; it != last; ++it)
        out[it->first - addr] = it->second;

    return {};
}

std::expected<void, BackendError> Linux64Backend::write_memory(address addr, std::span<const std::byte> in) {
    // writing over a breakpoint changes what's under its int3, the int3 itself stays
    std::vector<std::byte> buf(in.begin(), in.end());
    auto first = original_bytes_.lower_bound(addr);
    auto last = original_bytes_.lower_bound(addr + buf.size());
    for (auto it = first; it != last; ++it)
        buf[it->first - addr] = INT3;

    if (auto ok = access_memory(addr, buf.data(), buf.size(), true); !ok)
        return ok;

    for (auto it = first; it != last; ++it)
        it->second = in[it->first - addr];

    return {};
}

auto Linux64Backend::add_breakpoint(const location& loc) -> std::expected<BreakpointId, BackendError> {
    breakpoint bp{next_breakpoint_id_, loc, std::nullopt};

    if (pid_ != 0) {
        if (auto ok = check_stopped(); !ok)
            return std::unexpected(ok.error());

        auto addr = resolve(loc);
        if (!addr)
            return std::unexpected(addr.error());

        if (auto ok = insert_int3(*addr); !ok)
            return std::unexpected(ok.error());

        bp.address = *addr;
    }

    next_breakpoint_id_++;
    breakpoints_.push_back(bp);
    return bp.id;
}

std::expected<void, BackendError> Linux64Backend::remove_breakpoint(BreakpointId id) {
    auto bp = std::ranges::find(breakpoints_, id, &breakpoint::id);
    if (bp == breakpoints_.end())
        return std::unexpected(std::format("no breakpoint number {}", id));

    std::optional<address> addr = bp->address;
    if (addr) {
        if (auto ok = check_stopped(); !ok)
            return ok;
    }

    breakpoints_.erase(bp);

    // other breakpoints at the same address share the int3
    if (addr && std::ranges::find(breakpoints_, addr, &breakpoint::address) == breakpoints_.end())
        return remove_int3(*addr);

    return {};
}

auto Linux64Backend::breakpoints() const -> std::vector<breakpoint> {
    return breakpoints_;
}

std::expected<void, BackendError> Linux64Backend::load_executable(std::string_view path) {
    // symbols from whatever was loaded before shouldn't stick around if this fails
    symbols_.reset();
    load_offset_.reset();

    auto loaded = symbols::ElfSymbols::load(std::string(path));
    if (!loaded)
        return std::unexpected(loaded.error());

    symbols_ = std::move(*loaded);

    // non-PIE executables get loaded where they were linked, PIE ones only get a base once they're running
    if (!symbols_->pie()) {
        load_offset_ = 0;
    } else if (pid_ != 0) {
        if (auto offset = read_load_offset())
            load_offset_ = *offset;
    }

    return {};
}

auto Linux64Backend::symbolize(address addr) const -> std::optional<location> {
    if (!symbols_ || !load_offset_)
        return std::nullopt;

    // where it'd be without the load offset, which is what symbol values are
    address link_addr = addr - *load_offset_;
    auto sym = symbols_->containing(link_addr);
    if (!sym)
        return std::nullopt;

    return location{location::Symbol{sym->name, link_addr - sym->value}};
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
        auto event = handle_status(status);
        if (event && event_handler_)
            event_handler_(*event);
    }
}

// goes through /proc/pid/mem so we can do more than a word at a time like PTRACE_PEEKDATA
// (writes also ignore page protections, which breakpoints will need)
std::expected<void, BackendError> Linux64Backend::access_memory(address addr, std::byte *buf, std::size_t size, bool write) {
    if (auto ok = check_stopped(); !ok)
        return ok;

    // opened every time since the fd goes stale if the process execs
    int fd = open(std::format("/proc/{}/mem", pid_).c_str(), (write ? O_WRONLY : O_RDONLY) | O_CLOEXEC);
    if (fd == -1)
        return errno_error();

    std::size_t done = 0;
    while (done < size) {
        auto offset = static_cast<off_t>(addr + done);
        ssize_t n = write ? pwrite(fd, buf + done, size - done, offset) : pread(fd, buf + done, size - done, offset);
        if (n == -1 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return std::unexpected(std::format("cannot access memory at {:#x}", addr + done));
        }
        done += n;
    }

    close(fd);
    return {};
}

std::expected<void, BackendError> Linux64Backend::poke_byte(address addr, std::byte value) {
    return access_memory(addr, &value, 1, true);
}

// blocking wait that skips the event loop, for when we need the result right away
std::expected<StopEvent, BackendError> Linux64Backend::wait() {
    // handle_status swallows the stop at the end of a step over, so keep going until there's a real event
    while (true) {
        int status;
        int ret;
        do {
            ret = waitpid(pid_, &status, 0);
        } while (ret == -1 && errno == EINTR);

        if (ret == -1)
            return errno_error();

        if (auto event = handle_status(status))
            return *event;
    }
}

// empty when the stop was just part of stepping over a breakpoint, and the process is running again
std::optional<StopEvent> Linux64Backend::handle_status(int status) {
    if (WIFEXITED(status)) {
        forget_process();
        return StopEvent{StopEvent::Reason::Exited, WEXITSTATUS(status)};
    }

    if (WIFSIGNALED(status)) {
        forget_process();
        return StopEvent{StopEvent::Reason::Killed, WTERMSIG(status)};
    }

    int signal = WSTOPSIG(status);
    stopped_ = true;

    if (step_over_) {
        // done stepping off a breakpoint (or something interrupted it), put its int3 back
        address addr = *step_over_;
        step_over_.reset();
        bool then_continue = std::exchange(continue_after_step_over_, false);
        // if this fails there's nothing better to do than carry on without the breakpoint
        if (original_bytes_.contains(addr))
            std::ignore = poke_byte(addr, INT3);

        if (signal == SIGTRAP) {
            if (!then_continue)
                return StopEvent{StopEvent::Reason::Stopped, signal};

            if (ptrace(PTRACE_CONT, pid_, 0, 0) == 0) {
                stopped_ = false;
                return std::nullopt;
            }
        }
        // anything else gets reported like a normal stop
    }

    if (signal == SIGTRAP) {
        // pc is just past the int3, back up so the original instruction runs on resume
        if (auto addr = trapped_breakpoint(); addr && set_pc(*addr)) {
            auto bp = std::ranges::find(breakpoints_, addr, &breakpoint::address);

            // technically not strictly necessary (this should never fail),
            // but might as well make sure
            if (bp != breakpoints_.end())
                return StopEvent{StopEvent::Reason::Breakpoint, static_cast<int>(bp->id)};
        }
    }

    // signals get passed on to the process when it resumes, except:
    // - SIGTRAP, since stepping and exec report with it, and the program's own int3s get swallowed like in gdb
    // - SIGSTOP, which is usually from interrupt() (passing it on properly needs group-stop handling)
    // - SIGINT, which we reserve for frontend use (ctrl-c interrupt)
    if (signal != SIGTRAP && signal != SIGSTOP && signal != SIGINT)
        pending_signal_ = signal;

    return StopEvent{StopEvent::Reason::Stopped, signal};
}

void Linux64Backend::forget_process() {
    pid_ = 0;
    stopped_ = false;

    // a PIE executable could load somewhere else next time
    if (symbols_ && symbols_->pie())
        load_offset_.reset();

    // forget the patched bytes/int3 data now that process is gone
    original_bytes_.clear();
    step_over_.reset();
    continue_after_step_over_ = false;
    for (auto& bp : breakpoints_)
        bp.address.reset();
}

std::expected<void, BackendError> Linux64Backend::insert_int3(address addr) {
    // other breakpoints at the same address share the int3
    if (original_bytes_.contains(addr))
        return {};

    std::byte original;
    if (auto ok = access_memory(addr, &original, 1, false); !ok)
        return ok;

    if (auto ok = poke_byte(addr, INT3); !ok)
        return ok;

    original_bytes_.emplace(addr, original);
    return {};
}

std::expected<void, BackendError> Linux64Backend::remove_int3(address addr) {
    auto found = original_bytes_.find(addr);
    if (found == original_bytes_.end())
        return {};

    std::byte original = found->second;
    original_bytes_.erase(found);
    return poke_byte(addr, original);
}

bool Linux64Backend::at_breakpoint() {
    auto regs = get_registers();
    return regs && original_bytes_.contains(regs->pc());
}

// puts the original instruction back and single steps it
// the second half of this is done in handle_status, which puts the int3 back
std::expected<void, BackendError> Linux64Backend::start_step_over(bool then_continue) {
    auto regs = get_registers();
    if (!regs)
        return std::unexpected(regs.error());

    address addr = regs->pc();
    if (auto ok = poke_byte(addr, original_bytes_.at(addr)); !ok)
        return ok;

    if (ptrace(PTRACE_SINGLESTEP, pid_, 0, pending_signal_) == -1) {
        int err = errno;
        std::ignore = poke_byte(addr, INT3);
        return errno_error(err);
    }

    // these fields are used to store context for handle_status
    // PTRACE_SINGLESTEP raises another SIGTRAP, which will hit handle_status
    step_over_ = addr;
    continue_after_step_over_ = then_continue;
    stopped_ = false;
    pending_signal_ = 0;
    return {};
}

// address of the breakpoint if the current SIGTRAP came from one of our int3s
// int3 traps report SI_KERNEL, which tells them apart from a single step that lands just past a breakpoint
std::optional<Linux64Backend::address> Linux64Backend::trapped_breakpoint() {
    siginfo_t info;
    if (ptrace(PTRACE_GETSIGINFO, pid_, 0, &info) == -1 || info.si_code != SI_KERNEL)
        return std::nullopt;

    auto regs = get_registers();
    if (!regs)
        return std::nullopt;

    // pc is just past the int3
    address addr = regs->pc() - 1;
    if (!original_bytes_.contains(addr))
        return std::nullopt;

    return addr;
}

// load offset = AT_ENTRY (from aux vector) - e_entry (defined in ELF)
std::expected<Linux64Backend::address, BackendError> Linux64Backend::read_load_offset() {
    std::ifstream auxv(std::format("/proc/{}/auxv", pid_), std::ios::binary);
    Elf64_auxv_t entry;
    while (auxv.read(reinterpret_cast<char *>(&entry), sizeof(entry))) {
        if (entry.a_type == AT_ENTRY)
            return entry.a_un.a_val - symbols_->entry();
        if (entry.a_type == AT_NULL)
            break;
    }

    return std::unexpected(std::string("couldn't find the entry point in auxv"));
}

std::expected<void, BackendError> Linux64Backend::set_pc(address addr) {
    auto regs = get_registers();
    if (!regs)
        return std::unexpected(regs.error());

    // rip always exists, so this can't fail
    std::ignore = regs->set_register("rip", addr);
    return set_registers(*regs);
}

std::expected<void, BackendError> Linux64Backend::setup_child_events() {
    if (signal_fd_ != -1)
        return {};

    // SIGCHLD has to be blocked for signalfd to receive it
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (int err = pthread_sigmask(SIG_BLOCK, &mask, nullptr); err != 0)
        return errno_error(err);

    signal_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ == -1) {
        int err = errno;
        pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
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
