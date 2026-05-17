#ifndef FRONTENDS_CLI_H_
#define FRONTENDS_CLI_H_

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "backends/backend.h"
#include "event_loop.h"
#include "frontend.h"

namespace tdb::frontends {

/**
 * gdb style prompt on stdin/stdout, with line editing and history (libedit) when stdin is a terminal
 * input is only read while the process is stopped, so the process gets the terminal while it runs
 */
template<backends::Backend B>
class CliFrontend {
public:
    CliFrontend(EventLoop& loop, B& backend);
    CliFrontend(const CliFrontend&) = delete;
    CliFrontend& operator=(const CliFrontend&) = delete;
    ~CliFrontend();

    void start(Target target);

private:
    using Args = std::span<const std::string_view>;

    struct Command {
        std::string_view name;
        std::string_view alias;
        std::string_view usage;
        std::string_view help;
        // whether an empty line repeats it
        bool repeatable;
        // whether it takes a /modifier, e.g. "x/16"
        bool takes_modifier;
        void (CliFrontend::*handler)(std::string_view modifier, Args args);
    };

    static std::span<const Command> commands();

    static void readline_handler(char *line);

    void start_input();
    void stop_input();
    void handle_input();
    void handle_readline(char *line);
    void handle_pending_input();
    bool after_line();
    void handle_line(std::string_view line);
    void handle_event(backends::StopEvent event);
    void handle_interrupt();

    void prompt();
    void print_location();
    bool launch(Args args);
    void set_register(std::string_view name, std::string_view value_str);
    void set_memory(std::string_view modifier, std::string_view location_str, std::string_view value_str);
    void info_registers(Args names);
    void info_breakpoints();

    void cmd_run(std::string_view modifier, Args args);
    void cmd_starti(std::string_view modifier, Args args);
    void cmd_continue(std::string_view modifier, Args args);
    void cmd_stepi(std::string_view modifier, Args args);
    void cmd_kill(std::string_view modifier, Args args);
    void cmd_info(std::string_view modifier, Args args);
    void cmd_examine(std::string_view modifier, Args args);
    void cmd_set(std::string_view modifier, Args args);
    void cmd_break(std::string_view modifier, Args args);
    void cmd_delete(std::string_view modifier, Args args);
    void cmd_file(std::string_view modifier, Args args);
    void cmd_help(std::string_view modifier, Args args);
    void cmd_quit(std::string_view modifier, Args args);

    EventLoop& loop_;
    B& backend_;
    Target target_;
    // from the last run/starti, reused when they're given no args
    std::vector<std::string> args_;

    // libedit is global state, so its callback finds us through this (only one CliFrontend can use it)
    inline static CliFrontend *active_ = nullptr;
    // false when stdin isn't a terminal, then lines are read plainly instead of through libedit
    bool interactive_ = false;
    // set while reading commands, i.e. not while the process is running
    std::optional<EventLoop::Handle> input_watch_;
    // only used when not interactive, libedit does its own line buffering
    std::string pending_input_;
    std::string last_command_;
    // where the last x ended and how many bytes it showed, so x with no location can continue
    std::optional<typename B::address> next_examine_;
    std::size_t examine_count_ = 16;
    bool quitting_ = false;

    int interrupt_fd_ = -1;
    EventLoop::Handle interrupt_watch_;
};

} // tdb::frontends

#endif
