#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <print>
#include <ranges>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "backends/native.h"
#include "cli.h"

namespace tdb::frontends {

using backends::ProcessState;
using backends::StopEvent;

namespace {
std::string_view trim(std::string_view s) {
    auto start = s.find_first_not_of(" \t\r");
    if (start == std::string_view::npos)
        return {};
    auto end = s.find_last_not_of(" \t\r");
    return s.substr(start, end - start + 1);
}

std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> words;
    while (!(s = trim(s)).empty()) {
        auto end = s.find_first_of(" \t");
        words.push_back(s.substr(0, end));
        if (end == std::string_view::npos)
            break;
        s.remove_prefix(end);
    }
    return words;
}

// e.g. "SIGSEGV, Segmentation fault"
std::string describe_signal(int signal) {
    const char *abbrev = sigabbrev_np(signal);
    const char *descr = sigdescr_np(signal);
    if (!abbrev || !descr)
        return std::format("signal {}", signal);
    return std::format("SIG{}, {}", abbrev, descr);
}
} // anonymous namespace

template<backends::Backend B>
auto CliFrontend<B>::commands() -> std::span<const Command> {
    static constexpr std::array COMMANDS = {
        Command{"run", "r", "run [args...]", "start the program, optionally with new arguments", false, &CliFrontend::cmd_run},
        Command{"starti", "", "starti [args...]", "start the program and stop at the first instruction", false, &CliFrontend::cmd_starti},
        Command{"continue", "c", "continue", "resume the program", true, &CliFrontend::cmd_continue},
        Command{"stepi", "si", "stepi", "execute one instruction", true, &CliFrontend::cmd_stepi},
        Command{"kill", "k", "kill", "kill the program", false, &CliFrontend::cmd_kill},
        Command{"info", "i", "info registers [regs...]", "show registers (\"i r\" for short)", true, &CliFrontend::cmd_info},
        Command{"file", "", "file <path>", "set the program to debug", false, &CliFrontend::cmd_file},
        Command{"help", "h", "help", "show this message", false, &CliFrontend::cmd_help},
        Command{"quit", "q", "quit", "exit tdb, killing the program if it's running", false, &CliFrontend::cmd_quit},
    };
    return COMMANDS;
}

template<backends::Backend B>
CliFrontend<B>::CliFrontend(EventLoop& loop, B& backend) : loop_(loop), backend_(backend) {}

template<backends::Backend B>
CliFrontend<B>::~CliFrontend() {
    if (input_watch_)
        loop_.unwatch(*input_watch_);

    if (interrupt_fd_ != -1) {
        loop_.unwatch(interrupt_watch_);
        close(interrupt_fd_);

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
    }
}

template<backends::Backend B>
void CliFrontend<B>::start(Target target) {
    target_ = std::move(target);
    backend_.on_event([this](StopEvent event) { handle_event(event); });

    // ctrl-c goes to the whole foreground process group, so the process gets it too and stops by itself
    // we just need to not die, and reset the prompt if nothing is running
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) == 0) {
        interrupt_fd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (interrupt_fd_ != -1)
            interrupt_watch_ = loop_.watch_fd(interrupt_fd_, [this] { handle_interrupt(); });
        else
            pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
    }

    input_watch_ = loop_.watch_fd(STDIN_FILENO, [this] { handle_input(); });
    prompt();
}

template<backends::Backend B>
void CliFrontend<B>::handle_input() {
    // read the fd directly - std::cin would buffer lines that poll can't see
    char buf[256];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n == -1 && errno == EINTR)
        return;

    if (n <= 0) {
        // signifies user ctrl-d (EOF)
        std::println("quit");
        cmd_quit({});
        return;
    }

    pending_input_.append(buf, n);
    handle_pending_input();
}

template<backends::Backend B>
void CliFrontend<B>::handle_pending_input() {
    for (auto newline = pending_input_.find('\n'); newline != std::string::npos; newline = pending_input_.find('\n')) {
        std::string line = pending_input_.substr(0, newline);
        pending_input_.erase(0, newline + 1);
        handle_line(line);

        if (quitting_)
            return;

        // stop reading while the process runs, the rest gets handled once it stops
        if (backend_.state() == ProcessState::Running) {
            if (input_watch_) {
                loop_.unwatch(*input_watch_);
                input_watch_.reset();
            }
            return;
        }

        prompt();
    }
}

template<backends::Backend B>
void CliFrontend<B>::handle_line(std::string_view line) {
    std::string command_line(trim(line));
    if (command_line.empty())
        command_line = last_command_;
    if (command_line.empty())
        return;

    auto words = split(command_line);
    for (auto const& command : commands()) {
        if (words[0] != command.name && words[0] != command.alias)
            continue;

        last_command_ = command.repeatable ? command_line : "";
        (this->*command.handler)(Args(words).subspan(1));
        return;
    }

    last_command_ = "";
    std::println("Undefined command: \"{}\".  Try \"help\".", words[0]);
}

template<backends::Backend B>
void CliFrontend<B>::handle_event(StopEvent event) {
    switch (event.reason) {
    case StopEvent::Reason::Stopped:
        if (event.code != SIGTRAP)
            std::println("\nProgram received signal {}.", describe_signal(event.code));
        print_location();
        break;
    case StopEvent::Reason::Exited:
        if (event.code == 0)
            std::println("[Process exited normally]");
        else
            std::println("[Process exited with code {}]", event.code);
        break;
    case StopEvent::Reason::Killed:
        std::println("\nProgram terminated with signal {}.", describe_signal(event.code));
        break;
    }

    if (quitting_)
        return;

    if (!input_watch_)
        input_watch_ = loop_.watch_fd(STDIN_FILENO, [this] { handle_input(); });

    prompt();
    handle_pending_input();
}

template<backends::Backend B>
void CliFrontend<B>::handle_interrupt() {
    signalfd_siginfo info;
    while (read(interrupt_fd_, &info, sizeof(info)) > 0) {}

    // if the process is running it got the ctrl-c too, and we'll hear about it as a stop
    if (backend_.state() == ProcessState::Running)
        return;

    // the terminal throws away the half typed line, so do the same
    pending_input_.clear();
    std::println("Quit");
    prompt();
}

template<backends::Backend B>
void CliFrontend<B>::prompt() {
    std::print("(tdb) ");
    std::fflush(stdout);
}

template<backends::Backend B>
void CliFrontend<B>::print_location() {
    if (auto regs = backend_.get_registers())
        std::println("Stopped at {:#018x}", regs->pc());
    else
        std::println("{}", regs.error().message());
}

template<backends::Backend B>
bool CliFrontend<B>::launch(Args args) {
    if (backend_.state() != ProcessState::None) {
        std::println("The program is already running. Use \"kill\" first.");
        return false;
    }

    if (target_.path.empty()) {
        std::println("No program specified. Use \"file\" or pass it on the command line.");
        return false;
    }

    // new args replace the old ones for later runs
    if (!args.empty())
        args_.assign(args.begin(), args.end());

    std::print("Starting program: {}", target_.path);
    for (auto const& arg : args_)
        std::print(" {}", arg);
    std::println();

    if (auto ret = backend_.launch(target_.path, args_); !ret) {
        std::println("{}", ret.error().message());
        return false;
    }

    return true;
}

template<backends::Backend B>
void CliFrontend<B>::cmd_run(Args args) {
    if (!launch(args))
        return;

    if (auto ret = backend_.resume(); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::cmd_starti(Args args) {
    if (launch(args))
        print_location();
}

template<backends::Backend B>
void CliFrontend<B>::cmd_continue(Args) {
    if (auto ret = backend_.resume(); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::cmd_stepi(Args) {
    if (auto ret = backend_.step(); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::cmd_kill(Args) {
    if (auto ret = backend_.kill(); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::cmd_info(Args args) {
    if (args.empty() || (args[0] != "registers" && args[0] != "r")) {
        std::println("Usage: info registers [regs...]");
        return;
    }

    auto regs = backend_.get_registers();
    if (!regs) {
        std::println("{}", regs.error().message());
        return;
    }

    // name column fits the longest register name, value column fits a full width hex value
    std::size_t name_width = 0;
    for (auto [name, value] : *regs)
        name_width = std::max(name_width, name.size());

    auto print_register = [name_width](std::string_view name, auto value) {
        std::size_t hex_width = 2 + 2 * sizeof(value);
        std::println("{:<{}}  {:<#{}x}  {}", name, name_width, value, hex_width, value);
    };

    if (args.size() == 1) {
        for (auto [name, value] : *regs)
            print_register(name, value.get());
        return;
    }

    for (auto name : args.subspan(1)) {
        if (auto value = regs->get_register(name))
            print_register(name, *value);
        else
            std::println("Invalid register `{}'", name);
    }
}

template<backends::Backend B>
void CliFrontend<B>::cmd_file(Args args) {
    if (args.size() != 1) {
        std::println("Usage: file <path>");
        return;
    }

    target_.path = args[0];
}

template<backends::Backend B>
void CliFrontend<B>::cmd_help(Args) {
    // columns fit the longest entry, so adding commands doesn't break the alignment
    // yes, this does recompute every time help is called - but cost should be fairly small
    auto usage_width = std::ranges::max(commands() | std::views::transform([](auto const& command) { return command.usage.size(); }));
    auto alias_width = std::ranges::max(commands() | std::views::transform([](auto const& command) { return command.alias.size(); }));

    for (auto const& command : commands())
        std::println("{:<{}}  {:<{}}  {}", command.usage, usage_width, command.alias, alias_width, command.help);
}

template<backends::Backend B>
void CliFrontend<B>::cmd_quit(Args) {
    // backend kills the process when it gets destroyed
    quitting_ = true;
    loop_.stop();
}

template class CliFrontend<backends::NativeBackend>;
static_assert(Frontend<CliFrontend<backends::NativeBackend>, backends::NativeBackend>);

} // tdb::frontends
