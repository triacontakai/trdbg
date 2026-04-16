#include "backends/linux64.h"
#include "event_loop.h"
#include <array>
#include <print>
#include <string>
#include <unistd.h>

int main() {
    /*
    tdb::backends::Amd64Registers registers;

    auto ret = registers.set_register("rax", 727);
    if (!ret.has_value()) {
        std::println("error!");
        return 1;
    }

    std::println("{}", registers.get_register("rax").value());

    for (auto [name, val] : registers) {
        std::println("{}: {}", name, val.get());
    }
    */
    tdb::EventLoop loop;
    tdb::backends::Linux64Backend backend(loop);

    backend.on_event([&](tdb::backends::StopEvent event) {
        switch (event.reason) {
        case tdb::backends::StopEvent::Reason::Stopped:
            std::println("stopped by signal {}", event.code);
            break;
        case tdb::backends::StopEvent::Reason::Exited:
            std::println("exited with status {}", event.code);
            loop.stop();
            break;
        case tdb::backends::StopEvent::Reason::Killed:
            std::println("killed by signal {}", event.code);
            loop.stop();
            break;
        }
    });

    std::array<std::string, 1> args {"-al"};
    if (auto ret = backend.launch("./wow", args); !ret) {
        std::println("failure: {}", ret.error().message());
        return 1;
    }

    // stand-in for the frontend
    std::string pending_input;
    tdb::EventLoop::Handle stdin_watch = loop.watch_fd(STDIN_FILENO, [&] {
        // read the fd directly, std::cin would buffer lines that poll can't see
        char buf[256];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            // stop watching stdin but keep handling events
            loop.unwatch(stdin_watch);
            return;
        }

        pending_input.append(buf, n);
        for (auto newline = pending_input.find('\n'); newline != std::string::npos; newline = pending_input.find('\n')) {
            std::string line = pending_input.substr(0, newline);
            pending_input.erase(0, newline + 1);

            std::expected<void, tdb::backends::BackendError> ret;
            if (line == "c")
                ret = backend.resume();
            else if (line == "s")
                ret = backend.step();
            else if (line == "i")
                ret = backend.interrupt();
            else if (line == "k")
                ret = backend.kill();

            if (!ret)
                std::println("failure: {}", ret.error().message());
        }
    });

    if (auto ret = loop.run(); !ret) {
        std::println("failure: {}", ret.error().message());
        return 1;
    }

    return 0;
}
