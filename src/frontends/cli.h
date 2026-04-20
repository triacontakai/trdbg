#ifndef FRONTENDS_CLI_H_
#define FRONTENDS_CLI_H_

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
 * gdb style prompt on stdin/stdout
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
        void (CliFrontend::*handler)(Args args);
    };

    static std::span<const Command> commands();

    void handle_input();
    void handle_pending_input();
    void handle_line(std::string_view line);
    void handle_event(backends::StopEvent event);
    void handle_interrupt();

    void prompt();
    void print_location();
    bool launch(Args args);

    void cmd_run(Args args);
    void cmd_starti(Args args);
    void cmd_continue(Args args);
    void cmd_stepi(Args args);
    void cmd_kill(Args args);
    void cmd_info(Args args);
    void cmd_file(Args args);
    void cmd_help(Args args);
    void cmd_quit(Args args);

    EventLoop& loop_;
    B& backend_;
    Target target_;
    // from the last run/starti, reused when they're given no args
    std::vector<std::string> args_;

    std::optional<EventLoop::Handle> input_watch_;
    std::string pending_input_;
    std::string last_command_;
    bool quitting_ = false;

    int interrupt_fd_ = -1;
    EventLoop::Handle interrupt_watch_;
};

} // tdb::frontends

#endif
