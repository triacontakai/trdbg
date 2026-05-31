#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <editline/readline.h>
#include <utility>
#include <vector>
#include "backends/native.h"
#include "cli.h"
#include "parse.h"

namespace tdb::frontends {

using backends::ProcessState;
using backends::StopEvent;

namespace {
constexpr const char *PROMPT = "(tdb) ";

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

// undoes split(), for commands where the args are really one string
std::string join(std::span<const std::string_view> words) {
    std::string joined;
    for (auto word : words) {
        if (!joined.empty())
            joined += ' ';
        joined += word;
    }
    return joined;
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
        Command{"run", "r", "run [args...]", "start the program, optionally with new arguments", false, false, &CliFrontend::cmd_run},
        Command{"starti", "", "starti [args...]", "start the program and stop at the first instruction", false, false, &CliFrontend::cmd_starti},
        Command{"continue", "c", "continue", "resume the program", true, false, &CliFrontend::cmd_continue},
        Command{"stepi", "si", "stepi", "execute one instruction", true, false, &CliFrontend::cmd_stepi},
        Command{"kill", "k", "kill", "kill the program", false, false, &CliFrontend::cmd_kill},
        Command{"info", "i", "info registers [regs...]|breakpoints", "show registers or breakpoints (\"i r\", \"i b\" for short)", true, false, &CliFrontend::cmd_info},
        Command{"x", "", "x[/N] [addr|location]", "show N bytes of memory (default 16), continuing from the last x if no address", true, true, &CliFrontend::cmd_examine},
        Command{"set", "", "set[/N] $<reg>|<location> = <value>", "set a register, or N bytes of memory (default 8)", false, true, &CliFrontend::cmd_set},
        Command{"break", "b", "break <location>", "set a breakpoint", false, false, &CliFrontend::cmd_break},
        Command{"delete", "d", "delete [ids...]", "delete breakpoints, or all of them if no ids", false, false, &CliFrontend::cmd_delete},
        Command{"file", "", "file <path>", "set the program to debug", false, false, &CliFrontend::cmd_file},
        Command{"help", "h", "help", "show this message", false, false, &CliFrontend::cmd_help},
        Command{"quit", "q", "quit", "exit tdb, killing the program if it's running", false, false, &CliFrontend::cmd_quit},
    };
    return COMMANDS;
}

template<backends::Backend B>
CliFrontend<B>::CliFrontend(EventLoop& loop, B& backend) : loop_(loop), backend_(backend) {}

template<backends::Backend B>
CliFrontend<B>::~CliFrontend() {
    stop_input();
    if (active_ == this)
        active_ = nullptr;

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
    load_symbols();
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

    // readline-style editing needs a terminal, piped input (scripts, tests) gets read plainly
    interactive_ = isatty(STDIN_FILENO);
    if (interactive_) {
        active_ = this;
        // SIGINT comes through interrupt_fd_ instead
        rl_catch_signals = 0;
    }

    start_input();
}

template<backends::Backend B>
void CliFrontend<B>::readline_handler(char *line) {
    active_->handle_readline(line);
}

// shows the prompt and starts watching stdin, or just shows a fresh prompt if we already are
template<backends::Backend B>
void CliFrontend<B>::start_input() {
    if (input_watch_) {
        if (interactive_) {
            // libedit leaves the terminal in cooked mode after each line until the next key comes in,
            // so the prompt wouldn't show and keys would echo raw - reinstalling puts it back in edit mode now
            rl_callback_handler_remove();
            rl_callback_handler_install(PROMPT, &CliFrontend::readline_handler);
        } else {
            prompt();
        }
        return;
    }

    if (interactive_)
        rl_callback_handler_install(PROMPT, &CliFrontend::readline_handler);
    else
        prompt();

    input_watch_ = loop_.watch_fd(STDIN_FILENO, [this] { handle_input(); });
}

// gives the terminal back in its normal mode, for the process while it runs or for exiting
template<backends::Backend B>
void CliFrontend<B>::stop_input() {
    if (!input_watch_)
        return;

    if (interactive_)
        rl_callback_handler_remove();

    loop_.unwatch(*input_watch_);
    input_watch_.reset();
}

template<backends::Backend B>
void CliFrontend<B>::handle_input() {
    if (interactive_) {
        // reads one char, and calls readline_handler once there's a whole line
        rl_callback_read_char();
        return;
    }

    // read the fd directly - std::cin would buffer lines that poll can't see
    char buf[256];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n == -1 && errno == EINTR)
        return;

    if (n <= 0) {
        // signifies user ctrl-d (EOF)
        std::println("quit");
        cmd_quit({}, {});
        return;
    }

    pending_input_.append(buf, n);
    handle_pending_input();
}

template<backends::Backend B>
void CliFrontend<B>::handle_readline(char *line) {
    if (!line) {
        // ctrl-d on an empty line
        std::println("quit");
        cmd_quit({}, {});
        return;
    }

    std::string command_line(line);
    std::free(line);

    if (!trim(command_line).empty())
        add_history(command_line.c_str());

    handle_line(command_line);
    after_line();
}

template<backends::Backend B>
void CliFrontend<B>::handle_pending_input() {
    for (auto newline = pending_input_.find('\n'); newline != std::string::npos; newline = pending_input_.find('\n')) {
        std::string line = pending_input_.substr(0, newline);
        pending_input_.erase(0, newline + 1);
        handle_line(line);

        // the rest gets handled once the process stops
        if (!after_line())
            return;
    }
}

// returns whether we're still reading commands
template<backends::Backend B>
bool CliFrontend<B>::after_line() {
    if (quitting_)
        return false;

    // stop reading while the process runs, input starts again when it stops
    if (backend_.state() == ProcessState::Running) {
        stop_input();
        return false;
    }

    start_input();
    return true;
}

template<backends::Backend B>
void CliFrontend<B>::handle_line(std::string_view line) {
    std::string command_line(trim(line));
    if (command_line.empty())
        command_line = last_command_;
    if (command_line.empty())
        return;

    auto words = split(command_line);

    // "x/16" is command "x" with modifier "16"
    std::string_view name = words[0];
    std::string_view modifier;
    if (auto slash = name.find('/'); slash != std::string_view::npos) {
        modifier = name.substr(slash + 1);
        name = name.substr(0, slash);
    }

    for (auto const& command : commands()) {
        if (name != command.name && name != command.alias)
            continue;

        if (!modifier.empty() && !command.takes_modifier) {
            last_command_ = "";
            std::println("\"{}\" doesn't take a /modifier.", command.name);
            return;
        }

        last_command_ = command.repeatable ? command_line : "";
        (this->*command.handler)(modifier, Args(words).subspan(1));
        return;
    }

    last_command_ = "";
    std::println("Undefined command: \"{}\".  Try \"help\".", name);
}

template<backends::Backend B>
void CliFrontend<B>::handle_event(StopEvent event) {
    // the prompt can already be up if the process died while stopped (e.g. killed from outside)
    if (input_watch_ && interactive_)
        std::println();

    switch (event.reason) {
    case StopEvent::Reason::Stopped:
        if (event.code != SIGTRAP)
            std::println("\nProgram received signal {}.", describe_signal(event.code));
        print_location();
        break;
    case StopEvent::Reason::Breakpoint:
        std::println("\nBreakpoint {} hit.", event.code);
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

    start_input();
    if (!interactive_)
        handle_pending_input();
}

template<backends::Backend B>
void CliFrontend<B>::handle_interrupt() {
    signalfd_siginfo info;
    while (read(interrupt_fd_, &info, sizeof(info)) > 0) {}

    // if the process is running it got the ctrl-c too, and we'll hear about it as a stop
    if (backend_.state() == ProcessState::Running)
        return;

    // throw away the half typed line, like ctrl-c in a shell
    if (interactive_) {
        rl_replace_line("", 0);
        std::println("\nQuit");
        start_input();
    } else {
        pending_input_.clear();
        std::println("Quit");
        prompt();
    }
}

template<backends::Backend B>
void CliFrontend<B>::prompt() {
    std::print("{}", PROMPT);
    std::fflush(stdout);
}

template<backends::Backend B>
void CliFrontend<B>::print_location() {
    auto regs = backend_.get_registers();
    if (!regs) {
        std::println("{}", regs.error().message());
        return;
    }

    if (auto symbol = backend_.symbolize(regs->pc()))
        std::println("Stopped at {:#018x} <{}>", regs->pc(), *symbol);
    else
        std::println("Stopped at {:#018x}", regs->pc());
}

// not having symbols isn't fatal, addresses still work
template<backends::Backend B>
void CliFrontend<B>::load_symbols() {
    if (target_.path.empty())
        return;

    if (auto ret = backend_.load_executable(target_.path); !ret)
        std::println("No symbols loaded: {}", ret.error().message());
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
void CliFrontend<B>::cmd_run(std::string_view, Args args) {
    if (!launch(args))
        return;

    if (auto ret = backend_.resume(); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::cmd_starti(std::string_view, Args args) {
    if (launch(args))
        print_location();
}

template<backends::Backend B>
void CliFrontend<B>::cmd_continue(std::string_view, Args) {
    if (auto ret = backend_.resume(); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::cmd_stepi(std::string_view, Args) {
    if (auto ret = backend_.step(); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::cmd_kill(std::string_view, Args) {
    if (auto ret = backend_.kill(); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::cmd_info(std::string_view, Args args) {
    if (!args.empty() && (args[0] == "registers" || args[0] == "r"))
        info_registers(args.subspan(1));
    else if (!args.empty() && (args[0] == "breakpoints" || args[0] == "b"))
        info_breakpoints();
    else
        std::println("Usage: info registers [regs...]|breakpoints");
}

template<backends::Backend B>
void CliFrontend<B>::info_registers(Args names) {
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

    if (names.empty()) {
        for (auto [name, value] : *regs)
            print_register(name, value.get());
        return;
    }

    for (auto name : names) {
        if (auto value = regs->get_register(name))
            print_register(name, *value);
        else
            std::println("Invalid register `{}'", name);
    }
}

template<backends::Backend B>
void CliFrontend<B>::info_breakpoints() {
    auto breakpoints = backend_.breakpoints();
    if (breakpoints.empty()) {
        std::println("No breakpoints.");
        return;
    }

    std::vector<std::array<std::string, 3>> rows = {{"Num", "Location", "Address"}};
    std::size_t address_width = 2 + 2 * sizeof(typename B::address);
    for (auto const& bp : breakpoints) {
        rows.push_back({
            std::to_string(bp.id),
            std::format("{}", bp.location),
            bp.address ? std::format("{:#0{}x}", *bp.address, address_width) : "pending",
        });
    }

    // columns fit the longest entry
    std::size_t num_width = 0;
    std::size_t location_width = 0;
    for (auto const& row : rows) {
        num_width = std::max(num_width, row[0].size());
        location_width = std::max(location_width, row[1].size());
    }

    for (auto const& row : rows)
        std::println("{:<{}}  {:<{}}  {}", row[0], num_width, row[1], location_width, row[2]);
}

template<backends::Backend B>
void CliFrontend<B>::cmd_examine(std::string_view modifier, Args args) {
    // cap it so a typo doesn't try to dump gigabytes
    constexpr std::size_t MAX_COUNT = 65536;

    // count sticks around for later x's, same as gdb
    if (!modifier.empty()) {
        auto n = parse_integer<std::size_t>(modifier);
        if (!n || *n == 0 || *n > MAX_COUNT) {
            std::println("Invalid count `{}', must be 1 to {}", modifier, MAX_COUNT);
            return;
        }
        examine_count_ = *n;
    }

    typename B::address address;
    if (args.empty()) {
        if (!next_examine_) {
            std::println("Usage: x[/N] [addr|location]");
            return;
        }
        address = *next_examine_;
    } else if (auto number = parse_integer<typename B::address>(join(args))) {
        // plain numbers are addresses, anything else is a location
        address = *number;
    } else {
        auto location = backend_.parse_location(join(args));
        if (!location) {
            std::println("{}", location.error().message());
            return;
        }

        auto resolved = backend_.resolve(*location);
        if (!resolved) {
            std::println("{}", resolved.error().message());
            return;
        }
        address = *resolved;
    }

    std::vector<std::byte> bytes(examine_count_);
    if (auto ret = backend_.read_memory(address, bytes); !ret) {
        std::println("{}", ret.error().message());
        return;
    }

    // repeating with an empty line becomes a plain x, which continues from here
    next_examine_ = address + bytes.size();
    last_command_ = "x";

    // hexdump style, 16 bytes per row with the printable ones on the right
    constexpr std::size_t ROW_SIZE = 16;
    std::size_t address_width = 2 + 2 * sizeof(address);
    for (std::size_t row = 0; row < bytes.size(); row += ROW_SIZE) {
        std::string hex;
        std::string ascii;
        for (std::size_t i = 0; i < ROW_SIZE; i++) {
            if (i == ROW_SIZE / 2)
                hex += ' ';

            if (row + i >= bytes.size()) {
                hex += "   ";
                continue;
            }

            auto byte = std::to_integer<unsigned char>(bytes[row + i]);
            hex += std::format("{:02x} ", byte);
            ascii += (byte >= 0x20 && byte < 0x7f) ? static_cast<char>(byte) : '.';
        }

        std::println("{:#0{}x}:  {} |{}|", address + row, address_width, hex, ascii);
    }
}

template<backends::Backend B>
void CliFrontend<B>::cmd_set(std::string_view modifier, Args args) {
    // put the words back together so "$rax=1" and "$rax = 1" both work
    std::string assignment = join(args);

    auto equals = assignment.find('=');
    auto target = trim(std::string_view(assignment).substr(0, equals));
    if (equals == std::string::npos || target.empty()) {
        std::println("Usage: set[/N] $<reg>|<location> = <value>");
        return;
    }

    auto value_str = trim(std::string_view(assignment).substr(equals + 1));
    if (target.starts_with('$')) {
        if (!modifier.empty()) {
            std::println("Size can only be used with memory; must be omitted for registers");
            return;
        }
        set_register(target.substr(1), value_str);
    } else {
        set_memory(modifier, target, value_str);
    }
}

template<backends::Backend B>
void CliFrontend<B>::set_register(std::string_view name, std::string_view value_str) {
    auto regs = backend_.get_registers();
    if (!regs) {
        std::println("{}", regs.error().message());
        return;
    }

    auto value = parse_integer<typename B::registers::max_register_size>(value_str);
    if (!value) {
        std::println("Invalid value `{}'", value_str);
        return;
    }

    if (!regs->set_register(name, *value)) {
        std::println("Invalid register `{}'", name);
        return;
    }

    if (auto ret = backend_.set_registers(*regs); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::set_memory(std::string_view modifier, std::string_view location_str, std::string_view value_str) {
    std::size_t size = sizeof(typename B::address);
    if (!modifier.empty()) {
        auto n = parse_integer<std::size_t>(modifier);
        if (!n || (*n != 1 && *n != 2 && *n != 4 && *n != 8)) {
            std::println("Invalid size `{}', must be 1, 2, 4, or 8", modifier);
            return;
        }
        size = *n;
    }

    auto location = backend_.parse_location(location_str);
    if (!location) {
        std::println("{}", location.error().message());
        return;
    }

    auto address = backend_.resolve(*location);
    if (!address) {
        std::println("{}", address.error().message());
        return;
    }

    auto value = parse_integer<std::uint64_t>(value_str);
    if (!value) {
        std::println("Invalid value `{}'", value_str);
        return;
    }

    // little endian and truncated to size, fine while we only have x86 backends
    std::array<std::byte, sizeof(*value)> bytes;
    for (std::size_t i = 0; i < size; i++)
        bytes[i] = static_cast<std::byte>((*value >> (8 * i)) & 0xff);

    if (auto ret = backend_.write_memory(*address, std::span(bytes).first(size)); !ret)
        std::println("{}", ret.error().message());
}

template<backends::Backend B>
void CliFrontend<B>::cmd_break(std::string_view, Args args) {
    if (args.empty()) {
        std::println("Usage: break <location>");
        return;
    }

    auto location = backend_.parse_location(join(args));
    if (!location) {
        std::println("{}", location.error().message());
        return;
    }

    auto id = backend_.add_breakpoint(*location);
    if (!id) {
        std::println("{}", id.error().message());
        return;
    }

    if (backend_.state() == ProcessState::None)
        std::println("Breakpoint {} at {} (pending until the program starts)", *id, *location);
    else
        std::println("Breakpoint {} at {}", *id, *location);
}

template<backends::Backend B>
void CliFrontend<B>::cmd_delete(std::string_view, Args args) {
    if (args.empty()) {
        for (auto const& bp : backend_.breakpoints()) {
            if (auto ret = backend_.remove_breakpoint(bp.id); !ret)
                std::println("{}", ret.error().message());
        }
        return;
    }

    for (auto arg : args) {
        auto id = parse_integer<backends::BreakpointId>(arg);
        if (!id) {
            std::println("Invalid breakpoint number `{}'", arg);
            continue;
        }

        if (auto ret = backend_.remove_breakpoint(*id); !ret)
            std::println("{}", ret.error().message());
    }
}

template<backends::Backend B>
void CliFrontend<B>::cmd_file(std::string_view, Args args) {
    if (args.size() != 1) {
        std::println("Usage: file <path>");
        return;
    }

    target_.path = args[0];
    load_symbols();
}

template<backends::Backend B>
void CliFrontend<B>::cmd_help(std::string_view, Args) {
    // columns fit the longest entry, so adding commands doesn't break the alignment
    // yes, this does recompute every time help is called - but cost should be fairly small
    auto usage_width = std::ranges::max(commands() | std::views::transform([](auto const& command) { return command.usage.size(); }));
    auto alias_width = std::ranges::max(commands() | std::views::transform([](auto const& command) { return command.alias.size(); }));

    for (auto const& command : commands())
        std::println("{:<{}}  {:<{}}  {}", command.usage, usage_width, command.alias, alias_width, command.help);
}

template<backends::Backend B>
void CliFrontend<B>::cmd_quit(std::string_view, Args) {
    // backend kills the process when it gets destroyed
    quitting_ = true;
    stop_input();
    loop_.stop();
}

template class CliFrontend<backends::NativeBackend>;
static_assert(Frontend<CliFrontend<backends::NativeBackend>, backends::NativeBackend>);

} // tdb::frontends
